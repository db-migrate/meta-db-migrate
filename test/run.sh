#!/bin/sh
#
# The development cycle against real databases, once per driver: up, down,
# down again, up, check, reset, a migration that fails halfway, a dry run,
# and the launcher.
#
#   test/run.sh                    every driver
#   test/run.sh sqlite3 mysql      some of them
#
# Each driver runs the migrations in migrations/common and its own in
# migrations/<driver>. What the databases are and where is in database.json
# beside this; to start them:
#
#   docker run -d --rm --name dbm-meta-pg -e POSTGRES_PASSWORD=dbm \
#     -e POSTGRES_DB=dbm -p 127.0.0.1:55432:5432 postgres:12-alpine
#   docker run -d --rm --name dbm-meta-crdb -p 127.0.0.1:56257:26257 \
#     cockroachdb/cockroach:latest-v24.3 start-single-node --insecure
#   docker exec dbm-meta-crdb ./cockroach sql --insecure \
#     -e "create database dbm"
#   docker run -d --rm --name dbm-meta-mysql -e MYSQL_ROOT_PASSWORD=dbm \
#     -e MYSQL_DATABASE=dbm -p 127.0.0.1:53306:3306 mysql:8.0.28-oracle
#
# SQLite needs nothing but a file.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
drivers=${*:-pg cockroachdb mysql sqlite3}
failed=0

export DBM_TEST_PASSWORD=dbm
export PGPASSWORD=dbm

# ---------------------------------------------------------------- per driver

sql() {
  case $driver in
    pg)
      psql -h 127.0.0.1 -p 55432 -U postgres dbm -tAq -c "$1" 2>&1 ;;
    cockroachdb)
      psql "postgresql://root@127.0.0.1:56257/dbm?sslmode=disable" \
        -tAq -c "$1" 2>&1 ;;
    mysql)
      mysql -h127.0.0.1 -P53306 -uroot -pdbm dbm -N -B -e "$1" 2>/dev/null ;;
    sqlite3)
      sqlite3 "$work/dbm.sqlite" "$1" 2>&1 ;;
  esac
}

empty() {
  case $driver in
    pg)
      sql "drop schema public cascade; create schema public;" >/dev/null ;;
    cockroachdb)
      psql "postgresql://root@127.0.0.1:56257/defaultdb?sslmode=disable" -q \
        -c "drop database if exists dbm cascade" \
        -c "create database dbm" >/dev/null 2>&1 ;;
    mysql)
      sql "drop database if exists dbm; create database dbm;" ;;
    sqlite3)
      rm -f "$work/dbm.sqlite" ;;
  esac
}

# the tables there are, comma separated in name order
tables() {
  case $driver in
    pg|cockroachdb)
      sql "select coalesce(string_agg(tablename, ',' order by tablename), '')
           from pg_tables where schemaname = 'public'" ;;
    mysql)
      sql "select coalesce(group_concat(table_name order by table_name), '')
           from information_schema.tables where table_schema = 'dbm'" ;;
    sqlite3)
      sql "select coalesce(group_concat(name, ','), '') from (select name
           from sqlite_master where type = 'table' and name not like
           'sqlite_%' order by name)" ;;
  esac
}

# whether pets.traits says lazy for each pet, in order
lazy() {
  case $driver in
    pg|cockroachdb)
      sql "select string_agg(traits->>'lazy', ' ' order by id) from pets" ;;
    mysql)
      sql "select group_concat(json_extract(traits, '\$.lazy') order by id
           separator ' ') from pets" ;;
    sqlite3)
      sql "select group_concat(case json_type(traits, '\$.lazy') when 'true'
           then 'true' else 'false' end, ' ') from (select traits from pets
           order by id)" ;;
  esac
}

# what only this driver's own migration did, as one line
own() {
  case $driver in
    pg)
      sql "select format_type(atttypid, atttypmod) || ' ' ||
           (select count(*) from pg_class where relname = 'pet_tag_no') ||
           ' ' || (select string_agg(enumlabel, ',' order by enumsortorder)
           from pg_enum join pg_type on pg_type.oid = enumtypid
           where typname = 'pet_size')
           from pg_attribute where attrelid = 'pets'::regclass
           and attname = 'species'" ;;
    cockroachdb)
      sql "select (select count(*) from pets where name_length = length(name))
           || ' ' || (select data_type from information_schema.columns
           where table_name = 'events' and column_name = 'id') || ' ' ||
           (select string_agg(column_name, ',' order by seq_in_index)
           from information_schema.statistics where table_name = 'events'
           and index_name = 'events_pkey' and storing = 'NO')" ;;
    mysql)
      sql "select concat(column_type, ' ', (select column_type from
           information_schema.columns where table_schema = 'dbm' and
           table_name = 'audit' and column_name = 'note'), ' ',
           (select delete_rule from information_schema.referential_constraints
           where constraint_schema = 'dbm' and table_name = 'pets'))
           from information_schema.columns where table_schema = 'dbm'
           and table_name = 'audit' and column_name = 'id'" ;;
    sqlite3)
      sql "select (select \"table\" || ' ' || on_delete from
           pragma_foreign_key_list('toys')) || ' ' || (select count(*) from
           pragma_table_info('pets') where name = 'handle')" ;;
  esac
}

ownWanted() {
  case $driver in
    pg) echo "character varying(32) 1 small,large" ;;
    cockroachdb) echo "3 uuid kind,id" ;;
    mysql) echo "int unsigned mediumtext RESTRICT" ;;
    sqlite3) echo "pets CASCADE 1" ;;
  esac
}

# SQLite's own migration renames it, to show that it can
slugColumn() {
  case $driver in
    sqlite3) echo "handle" ;;
    *) echo "slug" ;;
  esac
}

# whether a database of that name exists on the server: 1 or 0
databaseExists() {
  case $driver in
    pg)
      sql "select count(*) from pg_database where datname = '$1'" ;;
    cockroachdb)
      sql "select count(*) from [show databases] where database_name = '$1'" ;;
    mysql)
      sql "select count(*) from information_schema.schemata
           where schema_name = '$1'" ;;
  esac
}

# a failure keeps its DDL on MySQL: it commits before every statement of it
halfDone() {
  case $driver in
    mysql) echo "half_done" ;;
    *) echo "" ;;
  esac
}

expect() {
  # $1 what, $2 wanted, $3 got
  if [ "$2" = "$3" ]; then
    echo "ok       $driver: $1"
  else
    echo "FAIL     $driver: $1"
    echo "  wanted: $2"
    echo "  got:    $3"
    failed=$((failed + 1))
  fi
}

# -------------------------------------------------------------- the cycle

"$top/build.sh" >/dev/null || exit 1

for driver in $drivers; do

  work="$top/build/test-$driver"
  failing="$top/build/test-$driver-failing"

  rm -rf "$work" "$failing"
  mkdir -p "$work/migrations" "$failing/migrations"

  for into in "$work" "$failing"; do
    cp "$here/main.c" "$here/database.json" "$into/"
    cp "$here"/migrations/common/*.c "$here/migrations/$driver"/*.c \
      "$into/migrations/"
    cp -r "$here/migrations/common/sqls" "$here/migrations/common/billing" \
      "$into/migrations/"
  done

  cp "$here"/migrations/failing/*.c "$failing/migrations/"
  ln -s "$work/dbm.sqlite" "$failing/dbm.sqlite"

  "$top/build-app.sh" "$work" "$work/app" "$driver" >"$work/build.log" 2>&1 ||
    { cat "$work/build.log"; exit 1; }
  "$top/build-app.sh" "$failing" "$failing/app" "$driver" \
    >"$failing/build.log" 2>&1 || { cat "$failing/build.log"; exit 1; }

  total=$(( $(ls "$work"/migrations/*.c | wc -l) +
             $(ls "$work"/migrations/sqls/*-up.sql | wc -l) ))
  app="$work/app"

  cd "$work"
  empty

  "$app" up -e "$driver" >"$work/up.out" 2>&1
  expect "up runs all $total" "$total" \
    "$(sql 'select count(*) from migrations')"
  expect "the rows are there" "kurbel,mausi,omalley" \
    "$(sql "select $(slugColumn) from pets order by id" | paste -sd, -)"
  expect "a comparison is a truth value" "false true false" "$(lazy)"
  expect "a migration in SQL, quotes and all" "it's; \"quoted\" fine" \
    "$(sql "select body from notes")"
  expect "and what only $driver does" "$(ownWanted)" "$(own)"

  # a scope is a directory of its own, and only `up:billing` runs it
  "$app" up:billing -e "$driver" >/dev/null 2>&1
  expect "up:scope runs the scope, recorded with its name" \
    "billing/20261008121000-invoices" \
    "$(sql "select name from migrations where name like 'billing/%'")"
  "$app" down -e "$driver" >/dev/null 2>&1
  expect "a plain down leaves the scope alone" 1 \
    "$(sql "select count(*) from migrations where name like 'billing/%'")"
  "$app" up -e "$driver" >/dev/null 2>&1
  "$app" down:billing -e "$driver" >/dev/null 2>&1
  expect "down:scope undoes it" 0 \
    "$(sql "select count(*) from migrations where name like 'billing/%'")"

  "$app" down -e "$driver" -c $((total - 1)) >/dev/null 2>&1
  expect "down -c undoes all but the first" 1 \
    "$(sql 'select count(*) from migrations')"
  expect "pets is gone" "migrations,migrations_state,owners" "$(tables)"

  "$app" up -e "$driver" >/dev/null 2>&1
  expect "up again" "$total" "$(sql 'select count(*) from migrations')"
  expect "and the data again" 3 "$(sql 'select count(*) from pets')"

  # destinations, compared on the timestamp the way node does
  "$app" sync -e "$driver" 20261008120100 >/dev/null 2>&1
  expect "sync down to a destination keeps it" 2 \
    "$(sql 'select count(*) from migrations')"
  "$app" up -e "$driver" 20261008120150 >/dev/null 2>&1
  expect "up to a destination includes it" 3 \
    "$(sql 'select count(*) from migrations')"
  "$app" sync -e "$driver" 99999999 >/dev/null 2>&1
  expect "sync up past the last runs all" "$total" \
    "$(sql 'select count(*) from migrations')"

  "$app" check -e "$driver" >"$work/check.out" 2>&1
  expect "check sees nothing pending" "[INFO] 0 migration(s) to run" \
    "$(tail -1 "$work/check.out")"

  "$app" reset -e "$driver" >/dev/null 2>&1
  expect "reset undoes everything" 0 "$(sql 'select count(*) from migrations')"
  expect "and leaves only its own tables" "migrations,migrations_state" "$(tables)"

  # the history in a table of another name, beside the usual one
  "$app" up -e "$driver" -t history -c 1 >/dev/null 2>&1
  expect "a history table of its own name" 1 \
    "$(sql 'select count(*) from history')"
  "$app" reset -e "$driver" -t history >/dev/null 2>&1
  sql "drop table history" >/dev/null

  cd "$failing"
  "$failing/app" up -e "$driver" >"$failing/up.out" 2>&1
  expect "a failing migration fails the run" 1 "$?"
  expect "the ones before it are recorded" "$total" \
    "$(sql 'select count(*) from migrations')"
  expect "the failing one is not" 0 \
    "$(sql "select count(*) from migrations where name like '%goes-wrong'")"
  expect "what is left of it" "$(halfDone)" \
    "$(tables | tr ',' '\n' | grep -x 'half_done' || true)"
  expect "its later steps never ran" "" \
    "$(tables | tr ',' '\n' | grep -x 'never_made' || true)"
  expect "the error is the database's own" yes \
    "$(grep -q 'table_that_does_not_exist' "$failing/up.out" && echo yes)"

  # without a transaction the steps before the failure stay, everywhere
  "$failing/app" up -e "$driver" --non-transactional >/dev/null 2>&1
  expect "--non-transactional keeps what ran before the failure" "half_done" \
    "$(tables | tr ',' '\n' | grep -x 'half_done' || true)"

  cd "$work"
  empty

  "$app" up -e "$driver" --dry-run >"$work/dry.out" 2>&1
  expect "a dry run changes nothing" "" "$(tables)"
  expect "and prints what it would do" 1 \
    "$(grep -c '^CREATE TABLE .pets.' "$work/dry.out")"

  # the launcher, loading this driver from its shared object
  rm -rf "$work/.meta-migrate"
  "$top/build/meta-migrate" up -e "$driver" >"$work/launch.out" 2>&1
  expect "the launcher runs all $total" "$total" \
    "$(sql 'select count(*) from migrations')"

  "$top/build/meta-migrate" down -e "$driver" >/dev/null 2>&1
  "$top/build/meta-migrate" up -e "$driver" >"$work/launch.out" 2>&1
  expect "and again from its cache" 0 \
    "$(grep -c '^\[INFO\] Compiling' "$work/launch.out")"

  "$app" check -e "$driver" >"$work/check.out" 2>&1
  expect "the shipped program agrees with it" \
    "[INFO] 0 migration(s) to run" "$(tail -1 "$work/check.out")"

  "$app" reset -e "$driver" >/dev/null 2>&1

  # db:create and db:drop, twice each, as node allows
  if [ "$driver" != sqlite3 ]; then
    "$app" db:create dbm_scratch -e "$driver" >/dev/null 2>&1
    "$app" db:create dbm_scratch -e "$driver" >/dev/null 2>&1
    expect "db:create makes one, and again is fine" "0 1" \
      "$? $(databaseExists dbm_scratch)"
    "$app" db:drop dbm_scratch -e "$driver" >/dev/null 2>&1
    "$app" db:drop dbm_scratch -e "$driver" >/dev/null 2>&1
    expect "db:drop removes it, and again is fine" "0 0" \
      "$? $(databaseExists dbm_scratch)"
  fi

  cd "$here"
done

# --------------------------------------- an edit, recompiled on its own

case " $drivers " in
  *" pg "*) ;;
  *) echo "$failed failed"; [ "$failed" -eq 0 ]; exit ;;
esac

driver=pg
work="$top/build/test-pg"
cd "$work"
empty

"$top/build/meta-migrate" up -e pg >/dev/null 2>&1
"$top/build/meta-migrate" down -e pg >/dev/null 2>&1
sed -i 's/increment: 10/increment: 5/' migrations/20261008120300-pg-only.c
"$top/build/meta-migrate" up -e pg >"$work/launch.out" 2>&1
expect "an edit recompiles only what changed" 1 \
  "$(grep -c '^\[INFO\] Compiling' "$work/launch.out")"
expect "and runs the edited version" 5 \
  "$(sql "select increment_by from pg_sequences where sequencename = 'pet_tag_no'")"
expect "under the name it always had" "/20261008120300-pg-only" \
  "$(sql 'select name from migrations order by id desc limit 1')"
"$top/build/meta-migrate" reset -e pg >/dev/null 2>&1
cd "$here"

echo "$failed failed"
[ "$failed" -eq 0 ]
