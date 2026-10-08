#!/bin/sh
#
# build-app.sh --static: a program that needs glibc and nothing else, and
# runs where none of its libraries are installed.
#
#   test/static.sh
#
# The static libraries come from tools/deps.sh, into build/deps unless
# DBM_DEPS has them. The program runs in bare Debian and Fedora containers
# against the PostgreSQL test/run.sh uses, and against SQLite, configured by
# YAML - all three statically linked.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-static"
deps=${DBM_DEPS:-$top/build/deps}
failed=0

export PGPASSWORD=dbm

expect() {
  if [ "$2" = "$3" ]; then
    echo "ok       $1"
  else
    echo "FAIL     $1"
    echo "  wanted: $2"
    echo "  got:    $3"
    failed=$((failed + 1))
  fi
}

"$top/build.sh" >/dev/null || exit 1
[ -f "$deps/deps.json" ] || "$top/tools/deps.sh" "$deps" || exit 1

rm -rf "$work"
mkdir -p "$work/app/migrations"
cd "$work"

printf '#include <db_migrate.h>\nint main(int c, char **v) { return dbmCli(c, v); }\n' \
  >app/main.c
cat >app/migrations/20261008200000-t.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("static_t", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("static_t"); }
DBM_MIGRATION(up, down)
EOF

DBM_DEPS=$deps "$top/build-app.sh" --static app ./app-static pg sqlite3 yaml \
  >build.out 2>&1 || cat build.out
expect "it builds" 1 "$(grep -c '^built' build.out)"

# what is left for the host to bring: the C library, its maths and loader
needs=$(ldd ./app-static | awk '{print $1}' | sed 's/\.so.*//' |
        grep -v '^linux-vdso' | sort | tr '\n' ' ')
expect "it needs glibc and nothing else" "/lib64/ld-linux-x86-64 libc libm " \
  "$needs"

psql -h 127.0.0.1 -p 55432 -U postgres -q \
  -c "drop database if exists static" -c "create database static" \
  >/dev/null 2>&1
cat >database.yml <<'EOF'
pg:
  driver: pg
  host: localhost
  port: 55432
  user: postgres
  password: dbm
  database: static
lite:
  driver: sqlite3
  filename: /tmp/static.db
EOF

for image in debian:13 fedora:latest; do
  docker run --rm --network host -v "$work/app-static:/app:ro" \
    -v "$work/database.yml:/w/database.yml:ro" -w /w "$image" \
    sh -c '/app up -e pg && /app down -e pg && /app up -e lite' \
    >"run.out" 2>&1
  expect "on bare $image: up and down on PostgreSQL, up on SQLite" 3 \
    "$(grep -c '^\[INFO\] Done' run.out)"
done

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
