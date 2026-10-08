#!/bin/sh
#
# Moving a project from node db-migrate to this, or back: the database one
# of them migrated is carried on by the other. Not the migrations - one has
# them in JavaScript, the other compiled - but what is in the database: the
# migrations table, and node's state, from which a v2 migration is undone.
#
#   test/compat.sh
#
# Needs node, and checkouts of node-db-migrate and its pg driver with their
# dependencies installed - beside this one, or where NODE_DB_MIGRATE and
# NODE_PG say - and the PostgreSQL that test/run.sh uses.
#
# Each direction: the old tool migrates the first two, the new one takes
# over - runs only the third, and undoes all three, the old tool's v2
# migration from what the old tool recorded.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
node_dbm=${NODE_DB_MIGRATE:-$top/../node-db-migrate}
node_pg=${NODE_PG:-$top/../pg}
work="$top/build/compat"
meta="$top/build/meta-migrate"
failed=0

export PGPASSWORD=dbm

sql() {
  psql -h 127.0.0.1 -p 55432 -U postgres compat -tAq -c "$1" 2>&1
}

tables() {
  sql "select coalesce(string_agg(tablename, ',' order by tablename), '')
       from pg_tables where schemaname = 'public'"
}

ran() {
  sql "select string_agg(name, ',' order by name) from migrations"
}

state() {
  sql "select string_agg(key, ',' order by key) from migrations_state"
}

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

fresh() {
  psql -h 127.0.0.1 -p 55432 -U postgres -q \
    -c "drop database if exists compat" -c "create database compat" \
    >/dev/null 2>&1
}

"$top/build.sh" >/dev/null || exit 1

rm -rf "$work"
mkdir -p "$work/node/migrations" "$work/meta/migrations"

# the same three migrations twice: for node, and in meta

# 1. v2: undone from what it learned
cat >"$work/node/migrations/20261008160000-pets.js" <<'EOF'
'use strict';
exports.migrate = async db => {
  await db.createTable('owners', {
    id: { type: 'int', primaryKey: true, autoIncrement: true },
    name: 'string'
  });
  await db.createTable('pets', {
    id: { type: 'int', primaryKey: true, autoIncrement: true },
    owner_id: {
      type: 'int',
      foreignKey: {
        name: 'pets_owner_fk', table: 'owners', mapping: 'id',
        rules: { onDelete: 'CASCADE' }
      }
    },
    name: { type: 'string', length: 48 }
  });
  await db.addIndex('pets', 'pets_name_idx', ['name']);
  await db.addColumn('pets', 'age', { type: 'int' });
  await db.renameColumn('pets', 'age', 'years');
};
exports._meta = { version: 2 };
EOF
cat >"$work/meta/migrations/20261008160000-pets.c" <<'EOF'
#include <db_migrate.h>

static int migrate(schema_t *db) {
  db->createTable("owners", {id: {type: "int", primaryKey: true,
                                  autoIncrement: true}, name: "string"});
  db->createTable("pets", {
    id: {type: "int", primaryKey: true, autoIncrement: true},
    owner_id: {type: "int", foreignKey: {name: "pets_owner_fk",
               table: "owners", mapping: "id", rules: {onDelete: "CASCADE"}}},
    name: {type: "string", length: 48},
  });
  db->addIndex("pets", "pets_name_idx", ["name"]);
  db->addColumn("pets", "age", {type: "int"});
  return db->renameColumn("pets", "age", "years");
}

DBM_MIGRATION_V2(migrate)
EOF

# 2. and 3. v1, with an up and a down
for pair in 20261008170000-tags:tags 20261008180000-visits:visits; do
  name=${pair%%:*}
  table=${pair##*:}
  cat >"$work/node/migrations/$name.js" <<EOF
'use strict';
exports.up = db => db.createTable('$table', { id: { type: 'int', primaryKey: true } });
exports.down = db => db.dropTable('$table');
exports._meta = { version: 1 };
EOF
  cat >"$work/meta/migrations/$name.c" <<EOF
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("$table", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("$table"); }
DBM_MIGRATION(up, down)
EOF
done

echo '{"name": "compat", "version": "1.0.0"}' >"$work/node/package.json"
cat >"$work/node/database.json" <<EOF
{"dev": {"driver": {"require": "$node_pg"}, "host": "127.0.0.1",
 "port": 55432, "user": "postgres", "password": "dbm", "database": "compat"}}
EOF
cat >"$work/meta/database.json" <<'EOF'
{"dev": {"driver": "pg", "host": "127.0.0.1", "port": 55432,
 "user": "postgres", "password": "dbm", "database": "compat"}}
EOF

run_node() { (cd "$work/node" && node "$node_dbm/bin/db-migrate" "$@"); }
run_meta() { (cd "$work/meta" && "$meta" "$@"); }

all="/20261008160000-pets,/20261008170000-tags,/20261008180000-visits"
everything="migrations,migrations_state,owners,pets,tags,visits"
empty="migrations,migrations_state"
internal="__dbmigrate_schema__,__dbmigrate_state__"

# one direction: $1 migrated so far, $2 takes over
transition() {
  old=$1
  new=$2

  fresh
  "run_$old" up 20261008170000 >/dev/null 2>&1
  expect "$old migrates the first two" \
    "migrations,migrations_state,owners,pets,tags" "$(tables)"

  "run_$new" up >"$work/$new.up" 2>&1
  expect "$new takes over and runs only the third" "$all $everything" \
    "$(ran) $(tables)"

  "run_$new" reset >"$work/$new.reset" 2>&1
  expect "and undoes all three, $old's v2 migration from $old's state" \
    "$empty" "$(tables)"
  expect "leaving the state as $old leaves it" "$internal" "$(state)"
}

echo "-- node db-migrate to this"
transition node meta
echo "-- this to node db-migrate"
transition meta node

echo "$failed failed"
[ "$failed" -eq 0 ]
