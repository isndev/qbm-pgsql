#!/usr/bin/env bash
# Spin up an ephemeral TLS + SCRAM-SHA-256 PostgreSQL for the qbm-pgsql SSL integration tests
# (integration/connection/connection-ssl.cpp + the SSL-side coverage cases). Reversible: `down` removes it.
#
#   ./tls-scram-pg.sh up      # start on :5433, print the QB_PG_SSL_DSN to export
#   ./tls-scram-pg.sh down     # stop + remove the container
#
# Then:  export QB_PG_SSL_DSN="tcp://test:test@localhost:5433[test]"  &&  ctest -L tier:integration
set -euo pipefail
NAME=qb-tls-pg ; PORT=5433
IMG="${QB_PG_IMAGE:-postgres:18}"   # any postgres>=14 with ssl support
DIR="$(cd "$(dirname "$0")" && pwd)/.tls"
case "${1:-up}" in
  up)
    mkdir -p "$DIR"
    [ -f "$DIR/server.crt" ] || openssl req -new -x509 -days 7 -nodes -text \
        -subj "/CN=localhost" -keyout "$DIR/server.key" -out "$DIR/server.crt" >/dev/null 2>&1
    chmod 600 "$DIR/server.key"
    docker rm -f "$NAME" >/dev/null 2>&1 || true
    docker run -d --name "$NAME" -p "${PORT}:5432" \
      -e POSTGRES_USER=test -e POSTGRES_PASSWORD=test -e POSTGRES_DB=test \
      -e POSTGRES_INITDB_ARGS="--auth-host=scram-sha-256" \
      -e POSTGRES_HOST_AUTH_METHOD=scram-sha-256 \
      -v "$DIR/server.crt:/var/lib/postgresql/server.crt:ro" \
      -v "$DIR/server.key:/var/lib/postgresql/server.key:ro" \
      "$IMG" -c ssl=on -c ssl_cert_file=/var/lib/postgresql/server.crt \
      -c ssl_key_file=/var/lib/postgresql/server.key -c password_encryption=scram-sha-256 >/dev/null
    sleep 6
    echo "TLS+SCRAM postgres up on :${PORT}. Export:"
    echo "  export QB_PG_SSL_DSN=\"tcp://test:test@localhost:${PORT}[test]\"" ;;
  down) docker rm -f "$NAME" >/dev/null 2>&1 || true ; echo "removed $NAME" ;;
  *) echo "usage: $0 up|down" ; exit 1 ;;
esac
