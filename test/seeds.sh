#!/bin/sh
#
# Static seeds as node 1.3.0 has them: rows flagged with the seed that
# inserted them, removed again before the seed runs anew, by `seed down`
# and `seed reset`, and remembered in __dbmigrate_seeds__.
#
#   test/seeds.sh
#
# SQLite, so it needs nothing running.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-seeds"
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
  sqlite3 "$work/dbm.sqlite" "$1" 2>&1
}

rows() {
  lite "select coalesce(group_concat(row, ' '), '') from (select $2 as row
    from $1 order by 1)"
}

seeded() {
  lite "select value from migrations_state where key = '__dbmigrate_seeds__'"
}

run() {
  (cd "$work" && "$meta" "$@")
}

rm -rf "$work"
mkdir -p "$work/migrations" "$work/seeds"
cd "$work"
echo '{"dev": {"driver": "sqlite3", "filename": "dbm.sqlite"}}' >database.json

cat >migrations/20260101000000-tables.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("owners", {
    id:   {type: "int", primaryKey: true},
    name: {type: "string", length: 20},
  });
  return db->createTable("pets", {
    id:       {type: "int", primaryKey: true, autoIncrement: true},
    owner_id: {type: "int", foreignKey: {name: "pets_owner", table: "owners",
               mapping: "id", rules: {onDelete: "RESTRICT"}}},
    name:     {type: "string", length: 20},
  });
}
DBM_MIGRATION_V2(migrate)
EOF
cat >migrations/20260101000001-plain.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) { return db->runSql("CREATE TABLE plain (id int)"); }
static int down(migrator_t *db) { return db->runSql("DROP TABLE plain"); }
DBM_MIGRATION(up, down)
EOF
run up >/dev/null 2>&1

cat >seeds/1-owners.c <<'EOF'
#include <db_migrate.h>
static int seed(seed_t *db) {
  return db->insert("owners", [{id: 1, name: "Ann"}, {id: 2, name: "Bob"}]);
}
DBM_SEED(seed)
EOF
cat >seeds/2-pets.c <<'EOF'
#include <db_migrate.h>
static int seed(seed_t *db) {
  json_t owners = db->all(SQL`SELECT id FROM owners WHERE id < 3 ORDER BY id`);
  for (int i = 0; i < owners.count(); ++i)
    db->insert("pets", {owner_id: owners.at(i).get("id"), name: "Rex"});
  return 0;
}
DBM_SEED(seed)
EOF
lite "insert into owners (id, name) values (9, 'hand')"

run seed >seed.out 2>&1
expect "rows are flagged with their seed" \
  "1|Ann|seed:1-owners 2|Bob|seed:1-owners 9|hand|" \
  "$(rows owners "id || '|' || name || '|' || coalesce(__dbmigrate__flag, '')")"
expect "a seed reads what the one before inserted" "1|seed:2-pets 2|seed:2-pets" \
  "$(rows pets "owner_id || '|' || __dbmigrate__flag")"
expect "and they are remembered as node does" '{"1-owners":["owners"],"2-pets":["pets"]}' \
  "$(seeded)"
expect "in node's words" "[INFO] [seed] 1-owners [INFO] [seed] 2-pets [INFO] Done " \
  "$(grep -o '\[INFO\] .*' seed.out | grep -v Compiling | tr '\n' ' ')"

# a changed seed: its rows replaced, the pets that point at them first
sed -i 's/"Bob"/"Bea"/' seeds/1-owners.c
run seed >/dev/null 2>&1
expect "a changed seed replaces its rows, a row by hand stays" "Ann Bea hand" \
  "$(rows owners name)"
expect "the seeds after it run again too" 2 "$(lite "select count(*) from pets")"

# a deleted seed's rows go with the next run
rm seeds/2-pets.c
run seed >/dev/null 2>&1
expect "a deleted seed's rows are removed" '0 {"1-owners":["owners"]}' \
  "$(lite "select count(*) from pets") $(seeded)"

# a seed by its name, and down by its name
cp seeds/1-owners.c seeds/3-more.c
sed -i 's/insert("owners", .*/insertColumns("owners", ["id", "name"], [ [3, "Cy"], [4, "Di"] ]);/' \
  seeds/3-more.c
run seed 3-more >/dev/null 2>&1
expect "seed <name> runs that one, insertColumns as node's (cols, rows)" \
  "Ann Bea Cy Di hand" "$(rows owners name)"
run seed down 3-more >/dev/null 2>&1
expect "seed down <name> removes its rows only" "Ann Bea hand" "$(rows owners name)"
expect "seed down it again is refused" 1 \
  "$(run seed down 3-more 2>&1 | grep -c 'There is no seeded seed "3-more"')"
expect "an unknown seed is refused" 1 \
  "$(run seed nope 2>&1 | grep -c 'There is no seed "nope"')"

run seed reset >/dev/null 2>&1
expect "seed reset removes them all, and remembers nothing" "hand {}" \
  "$(rows owners name) $(seeded)"
rm seeds/3-more.c

# a failing seed: what it inserted is removed by the next run
cat >seeds/2-fails.c <<'EOF'
#include <db_migrate.h>
#include <stdlib.h>
static int seed(seed_t *db) {
  db->insert("pets", []);
  db->insert("pets", {owner_id: 1, name: "Tom"});
  if (getenv("DBM_TEST_FAIL") != NULL)
    return db->fail(TEXT`stop here`);
  return 0;
}
DBM_SEED(seed)
EOF
DBM_TEST_FAIL=1 run seed >fail.out 2>&1
expect "a failing seed fails the run, its row stays for now" "1 1" \
  "$(grep -c 'stop here' fail.out) $(lite "select count(*) from pets")"
run seed >/dev/null 2>&1
expect "the next run removes it first" 1 "$(lite "select count(*) from pets")"
rm seeds/2-fails.c

# a table not created by a v2 migration
cat >seeds/4-plain.c <<'EOF'
#include <db_migrate.h>
static int seed(seed_t *db) { return db->insert("plain", {id: 1}); }
DBM_SEED(seed)
EOF
expect "a plain table is refused" 1 \
  "$(run seed 4-plain 2>&1 | grep -c 'seeds insert into tables created by v2 migrations only')"
rm seeds/4-plain.c

# the tables gone: reset has nothing to remove and does not fail
run seed >/dev/null 2>&1
run down -c 2 >/dev/null 2>&1
run seed reset >reset.out 2>&1
expect "reset after the tables were dropped" "0 {}" "$? $(seeded)"

# a program carries its seeds
run up >/dev/null 2>&1
printf '#include <db_migrate.h>\nint main(int c, char **v) { return dbmCli(c, v); }\n' >main.c
"$top/build-app.sh" . ./app sqlite3 >/dev/null 2>&1
mkdir -p run
cp app database.json run/
(cd run && ./app up >/dev/null 2>&1 && ./app seed >/dev/null 2>&1)
expect "a program carries its seeds inside it" "Ann Bea" \
  "$(sqlite3 run/dbm.sqlite "select group_concat(name, ' ') from (select name from owners order by name)")"

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
