#!/bin/sh
#
# v2 migrations that do not finish, as node handles them since
# 1.0.0-beta.38: a process that dies halfway, resumed by the next run -
# skipping what ran, or rolling it back - and a migration that fails,
# rolled back by exactly the steps that reached the database.
#
#   test/recovery.sh
#
# A process dies by killing itself (SIGKILL) where DBM_TEST_DIE is set, and
# leaves its lock behind; the next run takes it over after --lock-timeout.
# SQLite, and PostgreSQL for a foreign key that fails after its table was
# made, which SQLite cannot show (its keys are part of the table).
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-recovery"
meta="$top/build/meta-migrate"
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

lite() {
  sqlite3 "$1/dbm.sqlite" "$2" 2>&1
}

tables() {
  lite "$1" "select coalesce(group_concat(name, ','), '') from (select name
    from sqlite_master where type = 'table' and name not like 'sqlite_%'
    order by name)"
}

lock() {
  lite "$1" "select value from migrations_state where key = '__dbmigrate_state__'"
}

# a project with one v2 migration: two steps, the process dies, one more
project() {
  dir="$work/$1"
  mkdir -p "$dir/migrations"
  echo '{"dev": {"driver": "sqlite3", "filename": "dbm.sqlite"}}' \
    >"$dir/database.json"
  cat >"$dir/migrations/20261009120000-owners.c" <<EOF
#include <db_migrate.h>
#include <signal.h>
#include <stdlib.h>

static int migrate(schema_t *db) {
  db->createTable("owners", {id: {type: "int", primaryKey: true}});
  db->addColumn("owners", "name", {type: "string"});

  if (getenv("DBM_TEST_DIE") != NULL)
    raise(SIGKILL);

  return db->createTable("pets", {id: {type: "int", primaryKey: true}});
}

$2
EOF
}

run() {
  (cd "$1" && shift && "$meta" "$@" --lock-timeout 600 --lock-interval 100)
}

rm -rf "$work"
mkdir -p "$work"
cd "$work"

# ----------------------------------------------------------- skip, resumed

project skip "DBM_MIGRATION_V2(migrate)"
DBM_TEST_DIE=1 run "$work/skip" up >/dev/null 2>&1
expect "a process that dies halfway leaves the migration unfinished" \
  '"fin":0 "f":"20261009120000-owners" "o":"up" "done":2' \
  "$(lock "$work/skip" | grep -o '"fin":0\|"f":"[^"]*"\|"o":"[^"]*"\|"done":[0-9]*' | tr '\n' ' ' | sed 's/ $//')"

run "$work/skip" up >skip.out 2>&1
expect "the next run skips the steps that ran" 2 \
  "$(grep -c '\[recovery\] 20261009120000-owners: skipping already executed step' skip.out)"
expect "and runs the rest" "migrations,migrations_state,owners,pets" \
  "$(tables "$work/skip")"
expect "every undo step knows its step" "1,2,3" \
  "$(lite "$work/skip" "select value from migrations_state where key =
     '20261009120000-owners'" | grep -o '"n":[0-9]*' | cut -d: -f2 | sort | tr '\n' ',' | sed 's/,$//')"
run "$work/skip" down >/dev/null 2>&1
expect "and down undoes all of it" "migrations,migrations_state" \
  "$(tables "$work/skip")"

# --------------------------------------------------- rollback, run again

project rollback 'DBM_MIGRATION_V2_RECOVERY(migrate, "rollback")'
DBM_TEST_DIE=1 run "$work/rollback" up >/dev/null 2>&1
run "$work/rollback" up >rollback.out 2>&1
expect "a migration that says rollback is undone and run again" \
  "1 migrations,migrations_state,owners,pets" \
  "$(grep -c 'recovering by rollback' rollback.out) $(tables "$work/rollback")"
expect "and its lock row is finished" '"fin":1' \
  "$(lock "$work/rollback" | grep -o '"fin":1')"

# ------------------------------------------ changed since: no blind skip

project changed "DBM_MIGRATION_V2(migrate)"
DBM_TEST_DIE=1 run "$work/changed" up >/dev/null 2>&1
echo "/* edited */" >>"$work/changed/migrations/20261009120000-owners.c"
run "$work/changed" up >changed.out 2>&1
expect "a migration changed since is not skipped" "1 1" \
  "$? $(grep -c 'changed since, so the executed steps can not be skipped safely' changed.out)"
sed -i 's/^DBM_MIGRATION_V2(migrate)/DBM_MIGRATION_V2_RECOVERY(migrate, "rollback")/' \
  "$work/changed/migrations/20261009120000-owners.c"
run "$work/changed" up >/dev/null 2>&1
expect "but rolled back when it says so" "migrations,migrations_state,owners,pets" \
  "$(tables "$work/changed")"

# --------------------------- two columns of one table, both put back

dir="$work/columns"
mkdir -p "$dir/migrations"
echo '{"dev": {"driver": "sqlite3", "filename": "dbm.sqlite"}}' >"$dir/database.json"
cat >"$dir/migrations/20261009130000-pets.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->createTable("pets", {id: {type: "int", primaryKey: true},
                                  a: "string", b: "string", c: "string"});
}
DBM_MIGRATION_V2(migrate)
EOF
cat >"$dir/migrations/20261009130100-slim.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->removeColumn("pets", "a");
  db->removeColumn("pets", "b");
  return db->fail(TEXT`stop here`);
}
DBM_MIGRATION_V2(migrate)
EOF
run "$dir" up >columns.out 2>&1
expect "a failed migration that removed two columns of a table" \
  "1 __dbmigrate__flag,a,b,c,id" \
  "$(grep -c 'failed after step 2 removeColumn("pets", "b"): stop here' columns.out) $(lite "$dir" "select group_concat(name, ',') from (select name from pragma_table_info('pets') order by name)")"

# ------------------- PostgreSQL: a key that fails after its table is there

if psql -h 127.0.0.1 -p 55432 -U postgres -c '' >/dev/null 2>&1; then
  dir="$work/signal"
  mkdir -p "$dir/migrations"
  psql -h 127.0.0.1 -p 55432 -U postgres -q -c "drop database if exists recovery" \
    -c "create database recovery" >/dev/null 2>&1
  cat >"$dir/database.json" <<'EOF'
{"dev": {"driver": "pg", "host": "127.0.0.1", "port": 55432,
 "user": "postgres", "password": "dbm", "database": "recovery"}}
EOF
  cat >"$dir/migrations/20261009140000-keys.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("owners", {id: {type: "int", primaryKey: true}});
  return db->createTable("pets", {
    id: {type: "int", primaryKey: true},
    owner_id: {type: "int", foreignKey: {name: "pets_owner_fk",
               table: "nobody", mapping: "id"}},
  });
}
DBM_MIGRATION_V2(migrate)
EOF
  run "$dir" up >signal.out 2>&1
  expect "a table whose key failed is dropped by the rollback too" "" \
    "$(psql -h 127.0.0.1 -p 55432 -U postgres recovery -tAq -c \
      "select string_agg(tablename, ',') from pg_tables where schemaname = 'public'
       and tablename not like 'migrations%'")"
  expect "and the failure names the step and the statement" 2 \
    "$(grep -c 'failed at step 2 createTable("pets")\|SQL: ALTER TABLE' signal.out)"
else
  echo "skip     PostgreSQL on 55432 is not there"
fi

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
