<!-- Verified-against: qbm-pgsql @ qb 2.6.0 (C++20 default, C++23 supported) -->

# Contributing to qbm-pgsql

qbm-pgsql is a module of the qb actor framework. General contribution guidelines — branch and pull-request
flow, code style (`.clang-format` / `.clang-tidy`), the Developer Certificate of Origin sign-off, and the
Code of Conduct — follow the framework's [CONTRIBUTING guide](https://github.com/isndev/cube/blob/c++23/CONTRIBUTING.md). Security
vulnerabilities must not be filed as public issues; see [SECURITY.md](./SECURITY.md).

This document covers what is specific to building and testing the module.

## Build and test

qbm-pgsql is built through the qb module loader, not on its own. From a project that embeds qb:

```cmake
add_subdirectory(qb)
qb_load_modules("${CMAKE_CURRENT_SOURCE_DIR}/qbm")
target_link_libraries(your_target PRIVATE qbm::pgsql)
```

To build and run the module's test suite, configure the framework with tests enabled and build from the
repository root:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DQB_BUILD_TESTS=ON -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R qbm-pgsql
```

Integration tests need a reachable PostgreSQL server; they skip when one is not configured. Provide
connection details through the environment the tests document, and add a test for any new behavior. Exercise
both the success and the malformed-input paths when your change touches wire-protocol decoding.

## Conventions

Use the `qb::pg` namespace and include the module's umbrella header (the CMake link target is
`qbm::pgsql`, but the C++ namespace is `qb::pg`). Express time with the
`qb::duration` / `qb::wall_time` vocabulary: connect, statement, and transaction timeouts are
`qb::duration`, and `timestamptz` values are `qb::wall_time`. The PostgreSQL wire epoch (microseconds since

2000) is an internal encoding detail and stays native. Never use the removed `qb::Timestamp` /
      `qb::Duration` types.
