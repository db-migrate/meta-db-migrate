#!/bin/sh
#
# An index with what it says besides its columns - addIndexWith's unique,
# where, inverted and storing: written by the driver, refused where the
# database has no word for it, learned in node 1.8.2's keys and created
# again with all of it when a v2 migration is undone. test/run.sh does the
# same on PostgreSQL and CockroachDB, with the stored columns.
#
#   test/indexes.sh
#
# SQLite, so it needs nothing running.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-indexes"
meta="$top/build/meta-migrate"
failed=0

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
  sqlite3 "$work/$1/dbm.sqlite" "$2" 2>&1
}

# what SQLite keeps of an index: the statement that made it
index() {
  lite "$1" "select coalesce((select sql from sqlite_master where type = 'index'
    and name = '$2'), '')"
}

record() {
  lite "$1" "select value from migrations_state where key = '$2'"
}

project() {
  mkdir -p "$work/$1/migrations"
  echo '{"dev": {"driver": "sqlite3", "filename": "dbm.sqlite"}}' \
    >"$work/$1/database.json"
}

run() {
  (cd "$work/$1" && shift && "$meta" "$@" --lock-timeout 600 --lock-interval 100)
}

rm -rf "$work"
mkdir -p "$work"
cd "$work"

# ------------------------------------------------- v1: written as asked

project v1
cat >v1/migrations/20260101000000-pets.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  db->createTable("pets", {id: {type: "int", primaryKey: true},
                           name: "string", gone_at: "datetime"});
  db->addIndexWith("pets", "pets_live", ["name"],
                   {unique: true, where: "gone_at IS NULL"});
  return db->addIndexWith("pets", "pets_name", ["name"], {});
}
static int down(migrator_t *db) { return db->dropTable("pets"); }
DBM_MIGRATION(up, down)
EOF

run v1 up >v1.out 2>&1
expect "a unique partial index, its predicate as it was written" \
  'CREATE UNIQUE INDEX "pets_live" ON "pets" ("name") WHERE gone_at IS NULL' \
  "$(index v1 pets_live)"
expect "no options is a plain index" 'CREATE INDEX "pets_name" ON "pets" ("name")' \
  "$(index v1 pets_name)"

# what SQLite has no word for is refused, with the reason, before anything
# is sent - and so is an option nobody knows
refused() {
  sed "s|{unique: true, where: \"gone_at IS NULL\"}|$1|" \
    v1/migrations/20260101000000-pets.c >refused.c
  rm -rf refused && project refused
  cp refused.c refused/migrations/20260101000000-pets.c
  run refused up 2>&1 | grep -c "$2"
}

expect "storing is refused where there is none" 1 \
  "$(refused '{storing: ["gone_at"]}' 'index pets_live on pets: sqlite3 cannot store columns in an index (storing)')"
expect "inverted is refused where there is none" 1 \
  "$(refused '{inverted: true}' 'index pets_live on pets: sqlite3 has no inverted indexes')"
expect "an option nobody knows is refused" 1 \
  "$(refused '{uniqe: true}' 'index pets_live on pets: there is no index option "uniqe", only unique, inverted, where and storing')"
expect "options that are not an object are refused" 1 \
  "$(refused '["name"]' 'index pets_live on pets: its options are an object')"
expect "and nothing of the refused migration stays" "" \
  "$(lite refused "select name from sqlite_master where name = 'pets'")"

# ---------------------------------------- v2: learned and created again

project v2
cat >v2/migrations/20260101000000-pets.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("pets", {id: {type: "int", primaryKey: true},
                           name: "string", gone_at: "datetime"});
  db->addIndexWith("pets", "pets_live", ["name"],
                   {unique: true, where: "gone_at IS NULL"});
  return db->addUniqueIndex("pets", "pets_id", ["id"]);
}
DBM_MIGRATION_V2(migrate)
EOF
cat >v2/migrations/20260101000001-unindex.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->removeIndex("pets", "pets_live");
}
DBM_MIGRATION_V2(migrate)
EOF
cat >v2/migrations/20260101000002-drop.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->dropTable("pets");
}
DBM_MIGRATION_V2(migrate)
EOF

run v2 up >v2.out 2>&1
expect "the options are kept in node 1.8.2's keys" \
  '{"i":{"pets":{"pets_live":{"t":"pets","c":["name"],"u":true,"w":"gone_at IS NULL"}}},"c":{},"f":{},"s":[{"t":1,"a":"addIndex","c":["pets","pets_live"],"n":1}]}' \
  "$(record v2 20260101000001-unindex)"
expect "a plain unique index as u alone, kept with its table" \
  '{"i":{"pets":{"pets_id":{"t":"pets","c":["id"],"u":true}}},"c":{"pets":{"id":{"type":"int","primaryKey":true},"name":"string","gone_at":"datetime","__dbmigrate__flag":{"type":"string"}}},"f":{},"s":[{"t":1,"a":"createTable","c":["pets"],"n":1}]}' \
  "$(record v2 20260101000002-drop)"

run v2 down >/dev/null 2>&1
expect "a table restored gets its indexes with their options" \
  'CREATE UNIQUE INDEX "pets_id" ON "pets" ("id")' \
  "$(index v2 pets_id)"
expect "the one removed before stays removed" "" "$(index v2 pets_live)"

run v2 down >/dev/null 2>&1
expect "a removed index comes back with its predicate" \
  'CREATE UNIQUE INDEX "pets_live" ON "pets" ("name") WHERE gone_at IS NULL' \
  "$(index v2 pets_live)"

# a table restored by its rebuild, the index removed before it as well
cat >v2/migrations/20260101000003-both.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("toys", {id: {type: "int", primaryKey: true},
                           name: "string", kind: "string"});
  return db->addIndexWith("toys", "toys_kind", ["kind"],
                          {where: "kind <> 'ball'"});
}
DBM_MIGRATION_V2(migrate)
EOF
cat >v2/migrations/20260101000004-drop-toys.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->dropTable("toys");
}
DBM_MIGRATION_V2(migrate)
EOF
rm v2/migrations/20260101000002-drop.c
run v2 up >/dev/null 2>&1
run v2 down >/dev/null 2>&1
expect "a dropped table comes back with its partial index" \
  "CREATE INDEX \"toys_kind\" ON \"toys\" (\"kind\") WHERE kind <> 'ball'" \
  "$(index v2 toys_kind)"

# a stored column the table does not have is refused like an indexed one
cat >v2/migrations/20260101000005-unknown.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->addIndexWith("pets", "pets_cover", ["name"], {storing: ["fur"]});
}
DBM_MIGRATION_V2(migrate)
EOF
rm v2/migrations/20260101000004-drop-toys.c
expect "a stored column the schema does not know is refused" 1 \
  "$(run v2 up 2>&1 | grep -c 'The column "fur" of "pets" is unknown to the schema of db-migrate')"

# refused by the database's driver after it was learned: rolled back
cat >v2/migrations/20260101000005-unknown.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("cats", {id: {type: "int", primaryKey: true}, name: "string"});
  return db->addIndexWith("cats", "cats_cover", ["id"], {storing: ["name"]});
}
DBM_MIGRATION_V2(migrate)
EOF
run v2 up >v2-refused.out 2>&1
expect "an index the driver refuses fails the v2 migration" 1 \
  "$(grep -c 'sqlite3 cannot store columns in an index (storing)' v2-refused.out)"
expect "which is rolled back by the steps that ran" "" \
  "$(lite v2 "select name from sqlite_master where name = 'cats'")"
expect "and leaves the schema as it was" 0 \
  "$(record v2 __dbmigrate_schema__ | grep -c cats)"

# ---------------------------------------------------------- adopted

project adopt
cat >adopt/migrations/20260101000000-legacy.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  db->runSql("CREATE TABLE legacy (id INTEGER PRIMARY KEY, name VARCHAR(20))");
  return db->runSql("CREATE UNIQUE INDEX legacy_name ON legacy (name) WHERE name IS NOT NULL");
}
static int down(migrator_t *db) { return db->runSql("DROP TABLE legacy"); }
DBM_MIGRATION(up, down)
EOF
cat >adopt/migrations/20260101000001-adopt.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->adopt()->createTable("legacy", {id: {type: "int", primaryKey: true},
                                      name: {type: "string", length: 20}});
  db->adopt()->addIndexWith("legacy", "legacy_name", ["name"],
                            {unique: true, where: "name IS NOT NULL"});
  return db->removeIndex("legacy", "legacy_name");
}
DBM_MIGRATION_V2(migrate)
EOF
run adopt up >/dev/null 2>&1
expect "an adopted index is removed by the migration after it" "" \
  "$(index adopt legacy_name)"
run adopt down -c 1 >/dev/null 2>&1
expect "and comes back with the options it was adopted with" \
  'CREATE UNIQUE INDEX "legacy_name" ON "legacy" ("name") WHERE name IS NOT NULL' \
  "$(index adopt legacy_name)"

echo
echo "$failed failed"
[ "$failed" -eq 0 ]
