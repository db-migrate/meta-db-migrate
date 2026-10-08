#!/bin/sh
#
# node's options that are not about a database: --ignore-on-init,
# --log-level, and what a dry run reads. SQLite, since none of it depends on
# which database it is.
#
#   test/options.sh
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-options"
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

tables() {
  sqlite3 dbm.sqlite "select coalesce(group_concat(name, ','), '') from
    (select name from sqlite_master where type = 'table' and name not like
    'sqlite_%' order by name)" 2>&1
}

"$top/build.sh" >/dev/null || exit 1

rm -rf "$work"
mkdir -p "$work"
cd "$work"
echo '{"dev": {"driver": "sqlite3", "filename": "dbm.sqlite"}}' >database.json

# ---------------------------------------------------------- ignore-on-init

"$meta" create legacy --sql-file --ignore-on-init >/dev/null
up=$(ls migrations/sqls/*-legacy-up.sql)
expect "create --sql-file --ignore-on-init marks the up" \
  "-- db-migrate: ignore-on-init" "$(head -1 "$up")"
expect "and not the down" 0 \
  "$(grep -c ignore-on-init migrations/sqls/*-legacy-down.sql)"
echo "create table legacy (id int);" >>"$up"
echo "drop table legacy;" >"${up%-up.sql}-down.sql"

"$meta" create code --ignore-on-init >/dev/null
expect "a migration in code is written to look at db->ignoreOnInit" 1 \
  "$(grep -c 'if (db->ignoreOnInit)' migrations/*-code.c)"

# a database that has the table already, being taken over
sqlite3 dbm.sqlite "create table legacy (id int)"
"$meta" up --ignore-on-init >up.out 2>&1
expect "up --ignore-on-init records it without running it" \
  "1 legacy,migrations,migrations_state" \
  "$(grep -c 'ignoring on init' up.out) $(tables)"
"$meta" down -c 2 >/dev/null 2>&1
expect "and down undoes it as usual" "migrations,migrations_state" "$(tables)"
"$meta" up >/dev/null 2>&1
expect "without the option the up runs" "legacy,migrations,migrations_state" \
  "$(tables)"
"$meta" reset >/dev/null 2>&1

# --------------------------------------------------------------- log-level

expect "--log-level error leaves only errors" "" \
  "$("$meta" up --log-level error 2>&1)"
expect "--log-level warn hides a dry run's statements" "" \
  "$("$meta" down --dry-run --log-level warn 2>&1)"
expect "a dry run reads what has run: up offers nothing more" 1 \
  "$("$meta" up --dry-run 2>&1 | grep -c 'No migrations to run')"
expect "--log-level sql shows them" 1 \
  "$("$meta" down --dry-run --log-level sql 2>&1 | grep -c '^drop table legacy;')"
expect "--log-level info keeps -v from printing [SQL]" "0 1" \
  "$("$meta" down -v --log-level info 2>&1 | grep -c '^\[SQL\]') $("$meta" up -v --log-level info 2>&1 | grep -c '^\[INFO\] Done')"
expect "--ignore-completed-migrations is taken, as node takes it" 0 \
  "$("$meta" check --ignore-completed-migrations >/dev/null 2>&1; echo $?)"

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
