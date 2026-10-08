#!/bin/sh
#
# node db-migrate and this, against the same PostgreSQL database: the state
# one of them leaves is the state the other carries on from.
#
#   test/compat.sh
#
# Needs node, and the node-db-migrate and pg driver checkouts beside this one
# (NODE_DB_MIGRATE and NODE_PG to put them elsewhere), and the PostgreSQL that
# test/run.sh uses.
#
#   node up    -> meta down  -> node up      a v2 migration's learned state
#   meta up    -> node down  -> meta up      the same, the other way round
#   meta holds the lock      -> node waits   the lock is the same lock
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

rm -rf "$work"
mkdir -p "$work/node/migrations" "$work/meta/migrations"

psql -h 127.0.0.1 -p 55432 -U postgres -q \
  -c "drop database if exists compat" -c "create database compat" \
  >/dev/null 2>&1

# the same migration twice: once for node, once in meta
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

# a v1 migration that takes a while, for the lock
cat >"$work/meta/migrations/20261008170000-slow.c" <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) { return db->runSql("select pg_sleep(4)"); }
static int down(migrator_t *db) { (void)db; return 0; }
DBM_MIGRATION(up, down)
EOF
cat >"$work/node/migrations/20261008170000-slow.js" <<'EOF'
'use strict';
exports.up = db => db.runSql('select 1');
exports.down = () => Promise.resolve();
exports._meta = { version: 1 };
EOF

echo '{"name": "compat", "version": "1.0.0"}' >"$work/node/package.json"
cat >"$work/node/database.json" <<EOF
{"dev": {"driver": {"require": "$node_pg"}, "host": "127.0.0.1",
 "port": 55432, "user": "postgres", "password": "dbm", "database": "compat"}}
EOF
cat >"$work/meta/database.json" <<'EOF'
{"dev": {"driver": "pg", "host": "127.0.0.1", "port": 55432,
 "user": "postgres", "password": "dbm", "database": "compat"}}
EOF

node_up() { (cd "$work/node" && node "$node_dbm/bin/db-migrate" up "$@"); }
node_down() { (cd "$work/node" && node "$node_dbm/bin/db-migrate" down "$@"); }
meta_up() { (cd "$work/meta" && "$meta" up "$@"); }
meta_down() { (cd "$work/meta" && "$meta" down "$@"); }

# node's state, undone by this
node_up 20261008160000 >/dev/null 2>&1
expect "node migrates" "migrations,migrations_state,owners,pets" "$(tables)"
meta_down >/dev/null 2>&1
expect "this undoes node's v2 migration from node's state" \
  "migrations,migrations_state" "$(tables)"
expect "and leaves node's state as node leaves it" \
  "__dbmigrate_schema__,__dbmigrate_state__" \
  "$(sql "select string_agg(key, ',' order by key) from migrations_state")"
node_up 20261008160000 >/dev/null 2>&1
expect "on which node migrates again" "migrations,migrations_state,owners,pets" \
  "$(tables)"
node_down >/dev/null 2>&1

# this state, undone by node
meta_up 20261008160000 >/dev/null 2>&1
expect "this migrates" "migrations,migrations_state,owners,pets" "$(tables)"
node_down >/dev/null 2>&1
expect "node undoes this v2 migration from this state" \
  "migrations,migrations_state" "$(tables)"
meta_up 20261008160000 >/dev/null 2>&1
expect "on which this migrates again" "migrations,migrations_state,owners,pets" \
  "$(tables)"

# the lock: node waits while this holds it
meta_up --lock-timeout 3000 --lock-interval 300 >"$work/meta.out" 2>&1 &
sleep 1
node_up --lock-timeout 3000 --lock-interval 300 >"$work/node.out" 2>&1
wait
expect "node waits for the lock this holds" 1 \
  "$(grep -c 'Waiting for the migration lock' "$work/node.out")"
expect "and takes nothing over while the heartbeat beats" 0 \
  "$(grep -c 'stale' "$work/node.out")"
expect "and finds the work done when it gets it" 1 \
  "$(grep -c 'Nothing to run\|No migrations to run' "$work/node.out")"

echo "$failed failed"
[ "$failed" -eq 0 ]
