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

# ------------------------------------------- db-migrate-plugin-sql's format

"$meta" create users --sql >/dev/null
users=$(ls migrations/*-users.sql)
expect "create --sql writes the plugin's template" "-- up|||-- down||" \
  "$(tr '\n' '|' <"$users")"
printf -- '-- comments before it are fine\n-- UP\ncreate table users (id int);\n--  down\ndrop table users;\n' >"$users"
"$meta" up >/dev/null 2>&1
expect "its up section runs, the markers in any case" 1 \
  "$(sqlite3 dbm.sqlite "select count(*) from sqlite_master where name = 'users'")"
"$meta" down >/dev/null 2>&1
expect "and its down" 0 \
  "$(sqlite3 dbm.sqlite "select count(*) from sqlite_master where name = 'users'")"
printf 'select 1;\n-- up\nselect 2;\n' >"$users"
expect "SQL before the first section is refused, as the plugin does" 1 \
  "$("$meta" up 2>&1 | grep -c 'users.sql:1: SQL before the first section')"
printf -- '-- up\nselect 1;\n-- up\n' >"$users"
expect "and a section twice" 1 \
  "$("$meta" up 2>&1 | grep -c 'users.sql:3: the section "up" is defined twice')"
rm -f "$users"

# ------------------------------------- defaults for environment variables

mkdir -p envs/migrations
cat >envs/database.json <<'EOF'
{"defaultEnv": {"ENV": "DBM_TEST_ENV", "default": "lite"},
 "lite": {"driver": "sqlite3", "filename": {"ENV": "DBM_TEST_FILE", "default": "fallback.db"}},
 "fromurl": {"ENV": "DBM_TEST_URL", "default": "sqlite://x", "driver": "sqlite3", "filename": "url.db"}}
EOF
(cd envs && "$meta" check >/dev/null 2>&1 && DBM_TEST_FILE= "$meta" check >/dev/null 2>&1)
expect "a variable unset or empty takes its default, defaultEnv too" "fallback.db" \
  "$(cd envs && ls *.db)"
(cd envs && DBM_TEST_FILE=set.db "$meta" check >/dev/null 2>&1)
expect "a variable set wins" 1 "$(ls envs/set.db 2>/dev/null | wc -l)"
(cd envs && DBM_TEST_ENV=fromurl "$meta" check >/dev/null 2>&1)
expect "an environment from a URL keeps the keys beside it" 1 \
  "$(ls envs/url.db 2>/dev/null | wc -l)"
echo '{"defaultEnv": {"ENV": "DBM_TEST_NOPE"}, "development": {"driver": "sqlite3", "filename": "devel.db"}}' \
  >envs/database.json
(cd envs && "$meta" check >/dev/null 2>&1)
expect "a defaultEnv that stays empty is dev, or development" 1 \
  "$(ls envs/devel.db 2>/dev/null | wc -l)"

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
