#!/bin/sh
#
# Scopes as node 1.1.0 has them: `all` running the top level and every
# scope, nested ones too, and a scope's config.json - one that says more
# than a database or schema connecting on its own, one that says only that
# switching to it - with its lock and state kept where it connects.
#
#   test/scopes.sh
#
# SQLite, and PostgreSQL on 55432 for a scope switching to a schema.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-scopes"
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

tables() {
  sqlite3 "$1" "select coalesce(group_concat(name, ','), '') from (select name
    from sqlite_master where type = 'table' and name not like 'sqlite_%'
    order by name)" 2>&1
}

ran() {
  sqlite3 "$1" "select group_concat(name, ',') from (select name from
    migrations order by name)" 2>&1
}

migration() {
  # $1 file, $2 table
  cat >"$1" <<EOF
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("$2", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("$2"); }
DBM_MIGRATION(up, down)
EOF
}

rm -rf "$work"
mkdir -p "$work/p/migrations/billing" "$work/p/migrations/a/nested"
cd "$work/p"

echo '{"dev": {"driver": "sqlite3", "filename": "main.db"}}' >database.json
migration migrations/20261010000001-root.c root_t
migration migrations/billing/20261010000002-invoices.c invoices
migration migrations/a/nested/20261010000003-deep.c deep
# billing connects on its own: a file of its own, named by the environment
echo '{"filename": {"ENV": "BILLING_DB"}}' >migrations/billing/config.json

BILLING_DB=billing.db "$meta" up:all >all.out 2>&1
# scopes are those the program has migrations in - `a` has none of its own
expect "up:all enters the top level and every scope, nested ones too" \
  'Enter scope "/" Enter scope "a/nested" Enter scope "billing" ' \
  "$(grep -o 'Enter scope "[^"]*"' all.out | tr '\n' ' ')"
expect "the top level and a scope without a config share the database" \
  "/20261010000001-root,a/nested/20261010000003-deep" "$(ran main.db)"
expect "a scope with a connection of its own keeps its records there" \
  "billing/20261010000002-invoices" "$(ran billing.db)"
expect "and its state and lock" "invoices,migrations,migrations_state" \
  "$(tables billing.db)"

BILLING_DB=billing.db "$meta" down:all -c 5 >/dev/null 2>&1
expect "down:all undoes them in every scope" "migrations,migrations_state | migrations,migrations_state" \
  "$(tables main.db) | $(tables billing.db)"

# the same as a program, with no migrations directory beside it
printf '#include <db_migrate.h>\nint main(int c, char **v) { return dbmCli(c, v); }\n' >main.c
"$top/build-app.sh" . ./app sqlite3 >/dev/null 2>&1
mkdir -p "$work/run"
cp app database.json "$work/run/"
(cd "$work/run" && BILLING_DB=billing.db ./app up:all >/dev/null 2>&1)
expect "a program carries its scopes' configs inside it" \
  "/20261010000001-root,a/nested/20261010000003-deep | billing/20261010000002-invoices" \
  "$(ran "$work/run/main.db") | $(ran "$work/run/billing.db")"

"$meta" create:billing add-tax --sql-file >/dev/null 2>&1
expect "create:<scope> --sql-file writes into the scope's sqls/" 2 \
  "$(ls migrations/billing/sqls/*-add-tax-*.sql 2>/dev/null | wc -l)"

# ------------------------------------- PostgreSQL: a scope switching schema

if psql -h 127.0.0.1 -p 55432 -U postgres -c '' >/dev/null 2>&1; then
  psql -h 127.0.0.1 -p 55432 -U postgres -q \
    -c "drop database if exists scopes" -c "create database scopes" >/dev/null 2>&1
  psql -h 127.0.0.1 -p 55432 -U postgres scopes -q -c "create schema shop" >/dev/null 2>&1

  mkdir -p "$work/pg/migrations/shop"
  cd "$work/pg"
  cat >database.json <<'EOF'
{"dev": {"driver": "pg", "host": "127.0.0.1", "port": 55432,
 "user": "postgres", "password": "dbm", "database": "scopes"}}
EOF
  migration migrations/20261010000001-root.c root_t
  migration migrations/shop/20261010000002-orders.c orders
  echo '{"schema": "shop"}' >migrations/shop/config.json

  "$meta" up:all >pg.out 2>&1
  expect "a scope that only switches the schema migrates into it" \
    "public:migrations,migrations_state,root_t shop:migrations,migrations_state,orders" \
    "$(psql -h 127.0.0.1 -p 55432 -U postgres scopes -tAq -c \
      "select string_agg(schemaname || ':' || listed, ' ' order by schemaname) from
       (select schemaname, string_agg(tablename, ',' order by tablename) listed
        from pg_tables where schemaname in ('public', 'shop') group by schemaname) s")"
  expect "with its state in it" "shop/20261010000002-orders" \
    "$(psql -h 127.0.0.1 -p 55432 -U postgres scopes -tAq -c "select name from shop.migrations")"
else
  echo "skip     PostgreSQL on 55432 is not there"
fi

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
