#!/bin/sh
#
# Objects made outside v2 migrations, as node 1.1.0 handles them: adopted
# into the schema without sending anything, and dropped irreversibly when
# the schema does not know them.
#
#   test/adopt.sh
#
# SQLite, so it needs nothing running.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-adopt"
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

columns() {
  lite "$1" "select group_concat(name, ',') from (select name from
    pragma_table_info('$2') order by name)"
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

# ------------------------------------------------------------ adopting

project adopt
cat >adopt/migrations/20260101000000-legacy.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->runSql("CREATE TABLE legacy (id INTEGER PRIMARY KEY, name VARCHAR(20))");
}
static int down(migrator_t *db) { return db->runSql("DROP TABLE legacy"); }
DBM_MIGRATION(up, down)
EOF
cat >adopt/migrations/20260101000001-adopt.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->adopt()->createTable("legacy", {
    id:   {type: "int", primaryKey: true},
    name: {type: "string", length: 20},
  });
  db->addColumn("legacy", "age", {type: "int"});
  return db->removeColumn("legacy", "name");
}
DBM_MIGRATION_V2(migrate)
EOF

run adopt up >adopt.out 2>&1
expect "an adopted table is changed by the migration after it" "age,id" \
  "$(columns adopt legacy)"
expect "its record forgets the adoption and undoes the rest, as node's does" \
  '{"i":{},"c":{"legacy":{"name":{"type":"string","length":20}}},"f":{},"s":[{"t":2,"a":"dropTable","c":["legacy"],"n":1},{"t":0,"a":"removeColumn","c":["legacy","age"],"n":2},{"t":1,"a":"addColumn","c":["legacy","name",{}],"n":3}]}' \
  "$(record adopt 20260101000001-adopt)"
expect "an adopted table gets no __dbmigrate__flag" 0 \
  "$(record adopt __dbmigrate_schema__ | grep -c __dbmigrate__flag)"

run adopt down >/dev/null 2>&1
expect "down undoes the changes and leaves the adopted table" "id,name" \
  "$(columns adopt legacy)"
expect "and forgets it in the schema" 0 \
  "$(record adopt __dbmigrate_schema__ | grep -c legacy)"

# adopting what the schema knows already
cat >adopt/migrations/20260101000002-again.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  return db->adopt()->createTable("pets", {id: "int"});
}
DBM_MIGRATION_V2(migrate)
EOF
expect "adopting what the schema knows is refused" 1 \
  "$(run adopt up 2>&1 | grep -c 'createTable("pets") is known to the schema already, adopt is for objects created outside of v2 migrations only.')"
rm adopt/migrations/20260101000002-again.c

# ---------------------------------------------------- irreversible drops

project drop
cat >drop/migrations/20260101000000-junk.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) { return db->runSql("CREATE TABLE junk (id int)"); }
static int down(migrator_t *db) { return db->runSql("DROP TABLE IF EXISTS junk"); }
DBM_MIGRATION(up, down)
EOF
cat >drop/migrations/20260101000001-drop.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  return db->dropTable("junk");
}
DBM_MIGRATION_V2(migrate)
EOF
expect "a table the schema does not know is not dropped blind" 1 \
  "$(run drop up 2>&1 | grep -c 'The table "junk" is unknown to the schema of db-migrate, it was not created by a v2 migration. Declare it first with db.adopt.createTable("junk", columns), or pass { irreversible: true } to drop it without being able to revert it.')"

sed -i 's/db->dropTable("junk")/db->dropTableWith("junk", {irreversible: true})/' \
  drop/migrations/20260101000001-drop.c
run drop up >/dev/null 2>&1
expect "with { irreversible: true } it is dropped, and recorded as such" \
  '0 {"i":{},"c":{},"f":{},"s":[{"t":3,"a":"dropTable","c":["junk"],"n":1}]}' \
  "$(lite drop "select count(*) from sqlite_master where name = 'junk'") $(record drop 20260101000001-drop)"
expect "down refuses it, in node's words" 1 \
  "$(run drop down 2>&1 | grep -c 'Migration "20260101000001-drop" can not be reverted, it ran step 1 dropTable("junk") with { irreversible: true }.')"

# a failure after an irreversible step: no rollback, the next run continues
project stays
cp drop/migrations/20260101000000-junk.c stays/migrations/
cat >stays/migrations/20260101000001-drop.c <<'EOF'
#include <db_migrate.h>
#include <stdlib.h>
static int migrate(schema_t *db) {
  db->dropTableWith("junk", {irreversible: true});
  if (getenv("DBM_TEST_FAIL") != NULL)
    return db->fail(TEXT`stop here`);
  return db->createTable("kept", {id: {type: "int", primaryKey: true}});
}
DBM_MIGRATION_V2(migrate)
EOF
DBM_TEST_FAIL=1 run stays up >stays.out 2>&1
expect "a failure after it is not rolled back" "1 0" \
  "$(grep -c 'failed and can not be rolled back, it ran a step with { irreversible: true }' stays.out) $(lite stays "select count(*) from sqlite_master where name = 'junk'")"
run stays up >stays2.out 2>&1
expect "and the next run continues after the steps that ran" "1 1" \
  "$(grep -c 'skipping already executed step 1/1' stays2.out) $(lite stays "select count(*) from sqlite_master where name = 'kept'")"

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
