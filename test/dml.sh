#!/bin/sh
#
# dml migrations as node 1.4.0 has them: data changed by steps recorded with
# what reverts them - inserts by their flag, updates and deletes from backup
# tables, soft deletes by their mark - in batches, each in a transaction,
# continued after an interruption and rolled back after a failure.
#
#   test/dml.sh
#
# SQLite, so it needs nothing running. node's test/dml_test.js and the soft
# delete part of test/work_test.js, step for step.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-dml"
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

ORIGINAL="1|Rex|dog 2|Tom|cat 3|Nemo|fish 4|Bello|dog 5|Lassie|dog 6|Dory|fish"

lite() {
  sqlite3 "$work/$1/db" "$2" 2>&1
}

pets() {
  lite "$1" "select coalesce(group_concat(row, ' '), '') from (select id || '|'
    || coalesce(name, '') || '|' || coalesce(kind, '') as row from pets order by id)"
}

backups() {
  lite "$1" "select count(*) from sqlite_master where name like '__dbm_backup%'"
}

state() {
  lite "$1" "select value from migrations_state where key = '$2'"
}

run() {
  (cd "$work/$1" && shift && "$meta" "$@" --lock-timeout 600 --lock-interval 100)
}

# project <name> <migration body>...: m1 the tables, then each body, a dml
# migration unless it starts with "v2:"
project() {
  name=$1
  shift
  mkdir -p "$work/$name/migrations"
  echo '{"dev": {"driver": "sqlite3", "filename": "db"}}' >"$work/$name/database.json"
  cat >"$work/$name/migrations/20261009000001-m1.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("pets", {
    id:   {type: "int", primaryKey: true},
    name: "string",
    kind: "string",
    deleted_at: "datetime",
  });
  return db->createTable("logs", {line: "string"});
}
DBM_MIGRATION_V2(migrate)
EOF
  i=2
  for body in "$@"; do
    file="$work/$name/migrations/2026100900000$i-m$i.c"
    {
      echo '#include <db_migrate.h>'
      echo '#include <signal.h>'
      echo '#include <stdlib.h>'
      echo 'static int migrate(dml_t *db) {'
      echo "$body"
      echo '  return 0;'
      echo '}'
    } >"$file"
    case $body in
      *ROLLBACK_RECOVERY*) echo 'DBM_MIGRATION_DML_WITH(migrate, {recovery: "rollback"})' >>"$file" ;;
      *) echo 'DBM_MIGRATION_DML(migrate)' >>"$file" ;;
    esac
    i=$((i + 1))
  done
}

DATA='  db->insertColumns("pets", ["id", "name", "kind"], [ [1, "Rex", "dog"],
    [2, "Tom", "cat"], [3, "Nemo", "fish"], [4, "Bello", "dog"],
    [5, "Lassie", "dog"], [6, "Dory", "fish"] ]);'

rm -rf "$work"
mkdir -p "$work"
cd "$work"

# -------------------------------------- change data, restore it exactly

project change "$DATA" '
  db->updateWith("pets", {kind: "hound", name: null}, {kind: "dog"}, {batch: 2});
  db->deleteWith("pets", ["kind = '"'fish'"'"], {batch: 1});
  db->insert("pets", {id: 7, name: "Garfield", kind: "cat"});
  db->runSqlWith("UPDATE pets SET name = '"'TOM'"' WHERE id = ?", [2],
                 {revert: ["UPDATE pets SET name = ? WHERE id = 2", ["Tom"]]});
  db->update("pets", {name: "Kitty"}, {id: [7]});
  json_t rows = db->all(SQL`SELECT * FROM pets`);
  int count = rows.count();
  rows.release();
  if (count != 5)
    return db->fail(TEXT`all reads the rows`);'
run change up >change.out 2>&1
expect "up changes the data" "1||hound 2|TOM|cat 4||hound 5||hound 7|Kitty|cat" \
  "$(pets change)"
expect "with a backup for each update and delete" 3 "$(backups change)"
expect "recorded as node records it" \
  '{"i":{},"c":{},"f":{},"s":[{"t":4,"a":"update","c":["pets","__dbm_backup_038de134d4e911d5",["id"],["kind","name"]],"n":1,"b":1,"o":3},{"t":4,"a":"delete","c":["pets","__dbm_backup_6a47cb01bcbb4c20",["id"]],"n":2,"b":1,"o":2},{"t":4,"a":"insert","c":["pets","20261009000003-m3#3"],"n":3},{"t":4,"a":"runSql","c":["UPDATE pets SET name = ? WHERE id = 2",["Tom"]],"n":4},{"t":4,"a":"update","c":["pets","__dbm_backup_852f7948b8a5edb8",["id"],["name"]],"n":5,"b":1,"o":1}]}' \
  "$(state change 20261009000003-m3)"
expect "the backups registered" \
  '{"__dbm_backup_038de134d4e911d5":{"m":"20261009000003-m3","t":"pets","r":null},"__dbm_backup_6a47cb01bcbb4c20":{"m":"20261009000003-m3","t":"pets","r":null},"__dbm_backup_852f7948b8a5edb8":{"m":"20261009000003-m3","t":"pets","r":null}}' \
  "$(state change __dbmigrate_backups__)"
expect "the inserted row flagged with its step" "20261009000003-m3#3" \
  "$(lite change "select __dbmigrate__flag from pets where id = 7")"

run change down >/dev/null 2>&1
expect "down restores the rows exactly" "$ORIGINAL" "$(pets change)"
expect "and drops the backups, forgets them and the record" "0 {} " \
  "$(backups change) $(state change __dbmigrate_backups__) $(state change 20261009000003-m3)"
run change down >/dev/null 2>&1
expect "down of the insert removes its rows" "" "$(pets change)"

# -------------------------------------------- a failing migration rolls back

project failing "$DATA" '
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  db->insert("pets", {id: 7, name: "Garfield", kind: "cat"});
  db->insert("pets", {id: 1, name: "Rex again", kind: "dog"});'
run failing up -c 2 >/dev/null 2>&1
run failing up >failing.out 2>&1
expect "it fails at the step" 1 "$(grep -c 'at step 3 insert("pets")' failing.out)"
expect "said as node says it" 1 \
  "$(grep -c 'Migration "20261009000003-m3" failed, rolling back: ' failing.out)"
expect "and is rolled back: the rows, the backups, the record" "$ORIGINAL|0|0" \
  "$(pets failing)|$(backups failing)|$(lite failing "select count(*) from migrations where name like '%m3'")"

# -------------------------- an interrupted step continues where it stopped

# a failure after an irreversible step leaves the run unfinished: the next
# continues the update after its first batch, the one that went through
project continues "$DATA" '
  db->insertWith("logs", {line: "migrated"}, {irreversible: true});
  db->updateWith("pets", {kind: "hound"}, {kind: "dog"}, {batch: 1});'
run continues up -c 2 >/dev/null 2>&1
lite continues "create trigger stop before update on pets when new.id = 4
  begin select raise(fail, 'crash'); end"
run continues up >/dev/null 2>&1
expect "the batch before the failure stays" "hound cat fish dog dog fish" \
  "$(lite continues "select group_concat(kind, ' ') from (select kind from pets order by id)")"
lite continues "drop trigger stop"
run continues up >continues.out 2>&1
expect "the next run continues the step after it" "1 1" \
  "$(grep -c 'skipping already executed step 1/1' continues.out) $(grep -c 'continuing interrupted step 2 update("pets", {"kind":"hound"})' continues.out)"
expect "and changes the rest" "hound cat fish hound hound fish" \
  "$(lite continues "select group_concat(kind, ' ') from (select kind from pets order by id)")"
expect "down refuses it, in node's words" 1 \
  "$(run continues down 2>&1 | grep -c 'Migration "20261009000003-m3" can not be reverted, it ran step 1 insert("logs"), which can not be reverted.')"

# -------------------------------- irreversible steps stay and refuse reverting

project irreversible "$DATA" '
  db->insertWith("logs", {line: "migrated"}, {irreversible: true});
  db->updateWith("logs", {line: "x"}, {}, {irreversible: true});
  db->deleteWith("logs", {line: "nothing"}, {irreversible: true});
  db->runSql("UPDATE pets SET kind = '"'dog'"' WHERE id = 2", {irreversible: true});
  db->insert("pets", {id: getenv("DBM_TEST_FAIL") != NULL ? 1 : 7, name: "Garfield", kind: "cat"});'
run irreversible up -c 2 >/dev/null 2>&1
DBM_TEST_FAIL=1 run irreversible up >irreversible.out 2>&1
expect "a failure after them is not rolled back" "1 x dog" \
  "$(grep -c 'failed and can not be rolled back, it ran a step which can not be reverted' irreversible.out) $(lite irreversible "select group_concat(line) from logs") $(lite irreversible "select kind from pets where id = 2")"
run irreversible up >/dev/null 2>&1
expect "the next run continues after them" "x 7" \
  "$(lite irreversible "select group_concat(line) from logs") $(lite irreversible "select count(*) from pets")"
expect "down refuses" 1 "$(run irreversible down 2>&1 | grep -c 'can not be reverted')"

# ------------------------------------------ refuses what it can not revert

refused() {
  # $1 what, $2 the body, $3 the message
  rm -rf "$work/refused"
  project refused "$2"
  expect "refuses $1" 1 "$(run refused up 2>&1 | grep -q -- "$3" && echo 1)"
}
refused "an insert into a table v2 did not create" \
  'db->insert("unknown", {a: 1});' 'tables created by v2'
refused "a table without a known key" \
  'db->update("logs", {line: "a"}, {});' 'needs the primary key'
refused "changing the key" \
  'db->update("pets", {id: 9}, {id: 1});' 'can not change the key id'
refused "an update without values" \
  'db->update("pets", {}, {id: 1});' 'needs the values'
refused "a where of nothing it knows" \
  'db->delete("pets", [42]);' 'where is an object'
refused "runSql without its revert" \
  'db->runSql("DELETE FROM pets", {});' 'needs the SQL reverting it'
refused "columns and values that do not match" \
  'db->insertColumns("pets", ["id"], [1, 2]);' 'number of columns'
refused "an unknown mode" \
  'db->deleteWith("pets", {}, {mode: "hard"});' 'has no mode "hard"'
refused "a soft delete without its column" \
  'db->deleteWith("pets", {}, {mode: "soft"});' 'needs the column'

rm -rf "$work/refused"
project refused 'db->insert("pets", {id: 1});'
sed -i 's/DBM_MIGRATION_V2(migrate)/DBM_MIGRATION_V2_WITH(migrate, {type: "ddl"})/' \
  "$work/refused/migrations/20261009000001-m1.c"
expect "refuses an unknown type" 1 \
  "$(run refused up 2>&1 | grep -c 'Invalid migration type "ddl" in migration "20261009000001-m1"')"

# -------------------------------------------- more forms of insert, where, key

project forms '
  db->insert("pets", {columns: ["id", "name", "kind"], data: [1, "a", "x", 2, "b", null]});
  db->insert("pets", []);
  db->update("pets", {name: "c"}, ["kind = ?", ["x"]]);
  db->update("pets", {name: "d"}, {kind: null, id: []});
  db->deleteWith("pets", {kind: null}, {key: "id"});'
run forms up >/dev/null 2>&1
expect "every form of node's" "1|c|x" "$(pets forms)"
run forms down -c 1 >/dev/null 2>&1
expect "and down of them" "" "$(pets forms)"

# --------------------------------- an interrupted run recovered by rolling back

project rollback "$DATA" '
  /* ROLLBACK_RECOVERY */
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  if (getenv("DBM_TEST_DIE") != NULL)
    raise(SIGKILL);
  db->delete("pets", {kind: "fish"});'
run rollback up -c 2 >/dev/null 2>&1
DBM_TEST_DIE=1 run rollback up >/dev/null 2>&1
run rollback up --lock-timeout 1 >rollback.out 2>&1
expect "reverted first, then run again" "1 hound cat hound hound" \
  "$(grep -c 'recovering by rollback' rollback.out) $(lite rollback "select group_concat(kind, ' ') from (select kind from pets order by id)")"
run rollback down >/dev/null 2>&1
expect "and down restores it" "$ORIGINAL" "$(pets rollback)"

# ------------------------------------------------------------- a dry run

project dry "$DATA" '
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  db->delete("pets", {kind: "fish"});'
run dry up -c 2 >/dev/null 2>&1
run dry up --dry-run >dry.out 2>&1
expect "a dry run changes nothing and makes no backups" "$ORIGINAL 0" \
  "$(pets dry) $(backups dry)"
run dry up >/dev/null 2>&1
run dry down --dry-run >drydown.out 2>&1
expect "a dry down neither" "4 2" "$(lite dry "select count(*) from pets") $(backups dry)"
expect "it says what it would revert" 2 "$(grep -c '\[dml\] would revert' drydown.out)"

run dry fix >fix.out 2>&1
expect "fix skips dml migrations" "1 4" \
  "$(grep -c '\[fix\] skipping "20261009000003-m3", dml migrations change no schema' fix.out) $(lite dry "select count(*) from pets")"

# --------------------------------------------------------- soft deletes

project soft '
  db->insertColumns("pets", ["id", "name", "kind"], [ [1, "a", "dog"], [2, "b", "cat"], [3, "c", "dog"] ]);' '
  db->deleteWith("pets", {kind: "dog"}, {mode: "soft", column: "deleted_at"});'
run soft up >/dev/null 2>&1
expect "soft delete marks the rows and sets the column" \
  "1|20261009000002-m2#1|del:20261009000003-m3#1 2|20261009000002-m2#1 3|20261009000002-m2#1|del:20261009000003-m3#1" \
  "$(lite soft "select group_concat(row, ' ') from (select id || '|' || __dbmigrate__flag as row from pets order by id)")"
expect "with the time" 2 "$(lite soft "select count(*) from pets where deleted_at like '2%-%-% %:%:%'")"
run soft down -c 1 >/dev/null 2>&1
expect "down clears the column and the mark" "1||20261009000002-m2#1 2||20261009000002-m2#1 3||20261009000002-m2#1" \
  "$(lite soft "select group_concat(row, ' ') from (select id || '|' || coalesce(deleted_at, '') || '|' || __dbmigrate__flag as row from pets order by id)")"

# a purge, scheduled and then done by hand
cat >"$work/soft/migrations/20261009000003-m3.c" <<'EOF'
#include <db_migrate.h>
static int migrate(dml_t *db) {
  return db->deleteWith("pets", {kind: "dog"},
                        {mode: "soft", column: "deleted_at", value: "gone", purge: {releases: 1, drop: "auto"}});
}
DBM_MIGRATION_DML(migrate)
EOF
run soft up >/dev/null 2>&1
expect "a purge is scheduled" \
  '{"|del:20261009000003-m3#1":{"t":"pets","key":["id"],"r":null,"o":{"releases":1,"drop":"auto"}}}' \
  "$(state soft __dbmigrate_purges__)"
expect "the column set to the value given" 2 "$(lite soft "select count(*) from pets where deleted_at = 'gone'")"
cat >"$work/soft/migrations/20261009000004-m4.c" <<'EOF'
#include <db_migrate.h>
static int migrate(dml_t *db) {
  return db->purgeWith("pets", "20261009000003-m3", {batch: 1});
}
DBM_MIGRATION_DML(migrate)
EOF
run soft up >/dev/null 2>&1
expect "purge deletes them for good and forgets them" "2 {}" \
  "$(lite soft "select group_concat(id) from pets") $(state soft __dbmigrate_purges__)"
expect "and can not be reverted" 1 \
  "$(run soft down 2>&1 | grep -c 'it ran step 1 purge("pets"), which can not be reverted')"

# node's mark #1 is found in #10 too: here it is not
project marks '
  db->insertColumns("pets", ["id", "name", "kind"], [ [1, "a", "dog"], [2, "b", "cat"] ]);' '
  for (int i = 0; i < 9; ++i)
    db->runSql("SELECT 1", {revert: "SELECT 1"});
  db->deleteWith("pets", {id: 1}, {mode: "soft", column: "deleted_at"});'
run marks up >/dev/null 2>&1
lite marks "update pets set deleted_at = 'x', __dbmigrate__flag = '20261009000002-m2#1|del:20261009000003-m3#1' where id = 2"
run marks down -c 1 >/dev/null 2>&1
expect "reverting #10 leaves the mark #1 of another row" \
  "1|20261009000002-m2#1 2|20261009000002-m2#1|del:20261009000003-m3#1" \
  "$(lite marks "select group_concat(row, ' ') from (select id || '|' || __dbmigrate__flag as row from pets order by id)")"

# ------------------------- PostgreSQL, CockroachDB, MySQL: their own SQL

export PGPASSWORD=dbm

server() {
  # $1 driver, $2 statement: rows as `a|b` lines
  case $1 in
    pg) psql -h 127.0.0.1 -p 55432 -U postgres dml -tAq -c "$2" 2>&1 ;;
    cockroachdb) psql "postgresql://root@127.0.0.1:56257/dml?sslmode=disable" -tAq -c "$2" 2>&1 ;;
    mysql) mysql -h127.0.0.1 -P53306 -uroot -pdbm dml -N -B -e "$2" 2>/dev/null | tr '\t' '|' ;;
  esac
}

fresh() {
  case $1 in
    pg) psql -h 127.0.0.1 -p 55432 -U postgres -q -c "drop database if exists dml" \
          -c "create database dml" >/dev/null 2>&1 ;;
    cockroachdb) psql "postgresql://root@127.0.0.1:56257/defaultdb?sslmode=disable" -q \
          -c "drop database if exists dml cascade" -c "create database dml" >/dev/null 2>&1 ;;
    mysql) mysql -h127.0.0.1 -P53306 -uroot -pdbm -e \
          "drop database if exists dml; create database dml" 2>/dev/null ;;
  esac
}

for driver in pg cockroachdb mysql; do

  if ! fresh $driver || ! server $driver "select 1" | grep -q 1; then
    echo "skip     $driver is not there"
    continue
  fi

  project "on-$driver" "$DATA" '
  db->updateWith("pets", {kind: "hound", name: null}, {kind: "dog"}, {batch: 2});
  db->deleteWith("pets", {kind: "fish"}, {batch: 1});
  db->deleteWith("pets", {id: 2}, {mode: "soft", column: "deleted_at"});
  db->runSqlWith("UPDATE pets SET name = ? WHERE id = ?", ["Rexy", 1],
                 {revert: ["UPDATE pets SET name = ? WHERE id = ?", [null, 1]]});'
  case $driver in
    pg) echo '{"dev": {"driver": "pg", "host": "127.0.0.1", "port": 55432, "user": "postgres", "password": "dbm", "database": "dml"}}' ;;
    cockroachdb) echo '{"dev": {"driver": "cockroachdb", "host": "127.0.0.1", "port": 56257, "user": "root", "database": "dml"}}' ;;
    mysql) echo '{"dev": {"driver": "mysql", "host": "127.0.0.1", "port": 53306, "user": "root", "password": "dbm", "database": "dml"}}' ;;
  esac >"$work/on-$driver/database.json"

  rows="select concat(id, '|', coalesce(name, ''), '|', coalesce(kind, ''), '|',
    case when deleted_at is null then '' else 'deleted' end, '|',
    coalesce(__dbmigrate__flag, '')) from pets order by id"

  run "on-$driver" up >"on-$driver.out" 2>&1
  expect "$driver: up changes the data" \
    "1|Rexy|hound||20261009000002-m2#1 2|Tom|cat|deleted|20261009000002-m2#1|del:20261009000003-m3#3 4||hound||20261009000002-m2#1 5||hound||20261009000002-m2#1" \
    "$(server $driver "$rows" | tr '\n' ' ' | sed 's/ $//')"
  run "on-$driver" down >/dev/null 2>&1
  expect "$driver: down restores it exactly" \
    "1|Rex|dog||20261009000002-m2#1 2|Tom|cat||20261009000002-m2#1 3|Nemo|fish||20261009000002-m2#1 4|Bello|dog||20261009000002-m2#1 5|Lassie|dog||20261009000002-m2#1 6|Dory|fish||20261009000002-m2#1" \
    "$(server $driver "$rows" | tr '\n' ' ' | sed 's/ $//')"
  expect "$driver: and leaves no backups" 0 \
    "$(server $driver "select count(*) from information_schema.tables where table_name like '__dbm_backup%'")"
done

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
