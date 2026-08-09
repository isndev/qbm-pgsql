# `.github/ci/` — how qbm-pgsql tests itself

This directory exists because of one fact that is easy to rediscover the hard way:

> **qbm-pgsql cannot be configured on its own.**

```console
$ cmake -S . -B build
CMake Error at CMakeLists.txt:40 (message):
  [qbm-pgsql] this module cannot be configured on its own.
```

That `FATAL_ERROR` is deliberate — it fires before CMake reaches the first `qb_*` call, so the
symptom names the cause instead of reading like a missing `include()`.

## Why an installed qb does not fix it

`CMakeLists.txt` and `tests/CMakeLists.txt` call `qb_status_message()`, `qb_register_module()` and
`qb_register_module_test()`. Those live in **`qb/cmake/qbFunctions.cmake`** and are *development-time*
helpers: an installed qb ships `lib/cmake/qb/{qbConfig,qbConfigVersion,qbTargets}.cmake` plus the
`Find*.cmake` modules, and none of the `qb_*` commands. So

```console
$ cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/qb   # does NOT help
```

is the mistake the guard is there to name. There is no combination of `find_package(qb)` arguments
that makes a standalone configure work, and there is not meant to be — `find_package(qb)` is for
*consuming* qb, not for building a module's test tree.

## What does work

A minimal root that `add_subdirectory()`s a qb **source** tree first and this module second — which
is exactly what the (private) `qb-dev` superproject root does. That root is committed here:

    .github/ci/superbuild/CMakeLists.txt

It takes the two trees as cache variables instead of assuming a layout, and it is **byte-identical**
in `qbm-http`, `qbm-pgsql` and `qbm-redis` (nothing in it names a module). `qb-dev`'s
`dev/agent/verify.sh` asserts the three copies have not drifted.

### Reproduce the CI lane locally

```sh
git clone --depth 1 -b develop https://github.com/isndev/qb ../qb

cmake -S .github/ci/superbuild -B ../build-qbm-pgsql -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DQBM_CI_QB_DIR="$PWD/../qb" \
      -DQBM_CI_MODULE_DIR="$PWD"

# build this module's test binaries only (qb's own ~174 are qb's CI's business)
ctest --test-dir ../build-qbm-pgsql -N -L module:qbm-pgsql

cmake --build ../build-qbm-pgsql --parallel --target <the targets that listing named>
ctest --test-dir ../build-qbm-pgsql -L module:qbm-pgsql
```

Use the **branch of the same name**: qb, the three qbm modules and the superproject all carry
`main` (the released line) and `develop` (the next version) and move together.
`.github/workflows/tests.yml` resolves that automatically and proves the ref exists before checking
it out.

### Daemons — and why the CI postgres is not a `services:` container

14 of the 34 tests are `REQUIRES live`: they need a reachable PostgreSQL with the role, password and
database all `test` on `localhost:5432` (`tests/shared/test_config.hpp`), exported as `QB_PG_DSN`.

Two of them additionally need the server to have **TLS on**, and the official `postgres` image ships
`ssl=off`. That is why the workflow starts postgres with `docker run` after generating a self-signed
certificate rather than using a `services:` container: a service container starts *before* any step
runs, so the certificate could not exist yet.

`QB_PG_SSL_DSN` is pinned in the workflow on purpose. `connection-ssl` and `listen-notify` treat an
**unset** `QB_PG_SSL_DSN` as "TLS is not available here, skip"; with it set they **fail** instead.
That converts a soft skip into a hard gate, which is the whole point of running them in CI.

Locally:

```sh
docker run -d --name qb-pg -e POSTGRES_USER=test -e POSTGRES_PASSWORD=test -e POSTGRES_DB=test \
           -p 5432:5432 postgres:17-alpine
export QB_PG_DSN='tcp://test:test@localhost:5432[test]'
# leave QB_PG_SSL_DSN unset unless your server really has ssl=on
```

## One trap before you make this a required check

`tests.yml` carries `paths:` filters — the same shape `doc-lint.yml` here already uses — so a
pull request that touches only Markdown does not run it. That is deliberate: a docs-only PR cannot
break a test, and the lane costs a full build of qbm-pgsql's binaries.

It has a consequence, and it is the classic one. If `tests` is ever added to this branch's
protection rules as a **required** status check, a docs-only PR will sit forever on
*"Expected — Waiting for status to be reported"*, because a filtered-out workflow reports nothing
at all rather than reporting success. GitHub's answer is a companion job with the same check name
that always runs and passes when the real one is filtered out; this repository does not have one
yet. Decide that before turning the check on, not after.

## What this lane covers, and what it does not

It answers **"does qbm-pgsql still work with qb?"**. It does not answer *"do all three modules still
work together?"* — a qb-side change lands with no push here, so nothing in this repository fires for
it. The superproject's `qbm-tests.yml` remains the only lane that sees qb and all three modules at
once.

`ctest` counts `Skipped` as passed and still prints `100% tests passed`, so a green here is only
worth something because the workflow asserts, separately: a registration floor taken before the
build (the module total **and** the integration tier on its own), a non-zero translation-unit count,
an executable on disk for every registered test, an **executed** floor, and `skipped == 0`. With no
reachable PostgreSQL, 14 tests would self-skip, `ctest` would exit 0 and print `100% tests passed` —
that outcome fails this lane. A floor may only ever go **up**.

Unlike qbm-http, nothing here is host-varying: 34 registered (18 unit + 2 system + 14 integration) on
macOS and 34 on `ubuntu-latest`, measured. qbm-http has to derive part of its floor from a capability
because `ubuntu-latest` cannot build its four HTTP/3 tests. If a capability gate is ever added to this
module, its floor has to become derived in the same way — a constant would then be red on a host that
is merely different, not broken.
