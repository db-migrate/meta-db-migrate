#!/bin/sh
#
# Releases as node 1.5 to 1.7 have them: tables and columns deprecated in
# one release, renamed out of the way with the next and dropped later; rows
# deleted in soft mode purged and backups dropped once due; reverting the
# first migration of a release reverting what ran before it; fix learning
# it all again.
#
#   test/releases.sh
#
# SQLite, so it needs nothing running. node's test/release_test.js.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-releases"
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

# project <name>: a new one, its migrations added with `add`
project() {
  dir="$work/$1"
  n=0
  mkdir -p "$dir/migrations"
  echo '{"dev": {"driver": "sqlite3", "filename": "db"}}' >"$dir/database.json"
}

# add <body> [release] [dml]
add() {
  n=$((n + 1))
  file="$dir/migrations/2026100900000$n-m$n.c"
  [ "$n" -ge 10 ] && file="$dir/migrations/202610090000$n-m$n.c"
  if [ "${3:-}" = dml ]; then kind=dml_t; macro=DBM_MIGRATION_DML; else kind=schema_t; macro=DBM_MIGRATION_V2; fi
  {
    echo '#include <db_migrate.h>'
    echo "static int migrate($kind *db) {"
    echo "$1"
    echo '  return db->hasFailed() ? -1 : 0;'
    echo '}'
    if [ -n "${2:-}" ]; then
      echo "${macro}_WITH(migrate, {release: \"$2\"})"
    else
      echo "$macro(migrate)"
    fi
  } >"$file"
}

run() {
  (cd "$dir" && "$meta" "$@" --lock-timeout 600 --lock-interval 100)
}

lite() {
  sqlite3 "$dir/db" "$1" 2>&1
}

tables() {
  lite "select name from sqlite_master where type = 'table' and name not like
    'migrations%' and name != 'sqlite_sequence' order by name" |
    sed 's/_[0-9]*$/_T/' | tr '\n' ' ' | sed 's/ $//'
}

columns() {
  lite "select name from pragma_table_info('$1') order by name" |
    sed 's/_[0-9]*$/_T/' | tr '\n' ' ' | sed 's/ $//'
}

state() {
  lite "select value from migrations_state where key = '$1'"
}

rm -rf "$work"
mkdir -p "$work"
cd "$work"

# ------------------------------- rename with the next release, drop later

project tables
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  db->createTable("owners", {id: {type: "int", primaryKey: true}});' r1
add '  db->deprecateTableWith("pets", {releases: 2, drop: "auto"});'
run up >/dev/null 2>&1
expect "deprecated, the table stays" "owners pets" "$(tables)"
expect "marked in the schema as node marks it" \
  '{"tables":{"pets":{"r":"r1","to":"__dbm_deprecated_pets_20261009000002","o":{"releases":2,"drop":"auto"}}}}' \
  "$(state __dbmigrate_schema__ | sed 's/.*"d"://; s/}$//')"

add '  db->createTable("a", {id: "int"});' r2
run up >r2.out 2>&1
expect "the next release renames it" "__dbm_deprecated_pets_T a owners" "$(tables)"
expect "said as node says it" 2 \
  "$(grep -c '\[release\] starting release r2\|\[release\] renaming the deprecated table "pets" to __dbm_deprecated_pets_20261009000002' r2.out)"
expect "and recorded as a migration of its own" \
  '{"i":{},"c":{},"f":{},"s":[{"t":0,"a":"renameTable","c":["__dbm_deprecated_pets_20261009000002","pets"],"n":1}]}' \
  "$(state __dbmigrate_release__:r2)"

add '  db->createTable("b", {id: "int"});'
run up >/dev/null 2>&1
expect "a migration of the same release changes nothing" \
  "__dbm_deprecated_pets_T a b owners" "$(tables)"

add '  db->createTable("c", {id: "int"});' r3
run up >/dev/null 2>&1
expect "two releases later it is dropped" "a b c owners" "$(tables)"

run down -c 1 >/dev/null 2>&1
expect "reverting the first migration of a release reverts its steps" \
  "__dbm_deprecated_pets_T a b owners" "$(tables)"
run down -c 1 >/dev/null 2>&1
expect "one of the same release leaves them" "__dbm_deprecated_pets_T a owners" "$(tables)"
run down -c 1 >/dev/null 2>&1
expect "and the rename goes with r2" "owners pets" "$(tables)"
run down -c 1 >/dev/null 2>&1
run up >/dev/null 2>&1
expect "up again does it all again" "a b c owners" "$(tables)"

# --------------------------------- columns, relaxed, renamed, dropped by hand

project columns
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, name: "string"});' r1
add '  db->deprecateColumnWith("pets", "name", {releases: 1});'
run up >/dev/null 2>&1
add '  db->createTable("a", {id: "int"});' r2
run up >columns.out 2>&1
expect "the next release renames the column" "__dbm_deprecated_name_T __dbmigrate__flag id" \
  "$(columns pets)"
expect "due but manual: said only" 1 \
  "$(grep -c 'the deprecated column "name" of "pets" is due for dropping, deprecated 1 releases ago. Drop it in a migration with db.dropDeprecated("pets", "name").' columns.out)"
add '  db->dropDeprecated();'
run up >/dev/null 2>&1
expect "dropDeprecated drops what is due" "__dbmigrate__flag id" "$(columns pets)"
run down -c 1 >/dev/null 2>&1
expect "down brings it back renamed" "__dbm_deprecated_name_T __dbmigrate__flag id" \
  "$(columns pets)"
run down -c 2 >/dev/null 2>&1
expect "and under its name with the release" "__dbmigrate__flag id name" "$(columns pets)"

# --------------------------------------- the order counts, not the labels

project order
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  db->deprecateTableWith("pets", {releases: 1, drop: "auto"});' 9.0
add '  db->createTable("a", {id: "int"});' 1.0
run up >/dev/null 2>&1
expect "the order of the releases counts, not their labels" "a" "$(tables)"

# ---------------------------------- soft deleted rows purged with a release

project purge
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, deleted_at: "datetime"});' r1
add '  db->insert("pets", [{id: 1}, {id: 2}, {id: 3}]);
  db->deleteWith("pets", {id: [1, 2]}, {mode: "soft", column: "deleted_at", purge: {releases: 1, drop: "auto"}});' "" dml
run up >/dev/null 2>&1
expect "deleted in soft mode, the rows stay" 3 "$(lite "select count(*) from pets")"
add '  db->createTable("a", {id: "int"});' r2
run up >purge.out 2>&1
expect "the next release purges them" "3 {}" \
  "$(lite "select group_concat(id) from pets") $(state __dbmigrate_purges__)"
expect "said as node says it" 1 \
  "$(grep -c '\[release\] purging the rows of "pets" deleted in soft mode by 20261009000002-m2#2' purge.out)"
expect "and the release can not be reverted" 1 \
  "$(run down 2>&1 | grep -c 'Release r2 can not be reverted, it purged "pets" before its first migration.')"
expect "nothing was reverted" "a pets" "$(tables)"

project unpurged
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, deleted_at: "datetime"});' r1
add '  db->insert("pets", [{id: 1}]);
  db->deleteWith("pets", {id: 1}, {mode: "soft", column: "deleted_at", purge: {releases: 1, drop: "auto"}});' "" dml
run up >/dev/null 2>&1
run down >/dev/null 2>&1
expect "reverting a soft delete forgets its purge" "{}" "$(state __dbmigrate_purges__)"
add '  db->createTable("a", {id: "int"});' r2
run up -c 1 >/dev/null 2>&1
add '  db->deleteWith("pets", {}, {mode: "soft", column: "deleted_at", purge: "soon"});' "" dml
expect "purge takes true or options" 1 \
  "$(run up 2>&1 | grep -q 'takes purge: true or { releases, drop }' && echo 1)"

# ---------------------------------------------- the project's defaults

project defaults
echo '{"deprecation": {"releases": 1, "drop": "auto"}}' >"$dir/.db-migraterc"
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  db->deprecateTable("pets");' a
add '  db->createTable("a", {id: "int"});' b
run up >/dev/null 2>&1
expect "the project sets the defaults, in .db-migraterc" "a" "$(tables)"

project manualpurge
printf '[deprecation]\nreleases = 1\n' >"$dir/.db-migraterc"
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, deleted_at: "datetime"});' r1
add '  db->insert("pets", [{id: 1}, {id: 2}]);
  db->deleteWith("pets", {id: 1}, {mode: "soft", column: "deleted_at", purge: true});' "" dml
add '  db->createTable("a", {id: "int"});' r2
run up >manual.out 2>&1
expect "a manual purge is said to be due, by an INI .db-migraterc" "2 1" \
  "$(lite "select count(*) from pets") $(grep -c 'the rows of "pets" deleted in soft mode by 20261009000002-m2#2 are due for purging, deleted 1 releases ago. Purge them in a dml migration with db.purge("pets", "20261009000002-m2").' manual.out)"
add '  db->purge("pets", "20261009000002-m2");' "" dml
run up >manual2.out 2>&1
expect "until it is purged" "2 0" \
  "$(lite "select group_concat(id) from pets") $(grep -c 'due for purging' manual2.out)"

# ------------------------------------------------------------- backups

backups() {
  lite "select count(*) from sqlite_master where name like '__dbm_backup%'"
}

project final
echo '{"deprecation": {"releases": 1, "drop": "auto"}}' >"$dir/.db-migraterc"
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, kind: "string"});' r1
add '  db->insert("pets", [{id: 1, kind: "dog"}, {id: 2, kind: "cat"}]);
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  db->delete("pets", {kind: "cat"});' "" dml
run up >/dev/null 2>&1
expect "a dml migration keeps its backups" 2 "$(backups)"
add '  db->createTable("a", {id: "int"});' r2
run up >final.out 2>&1
expect "they are dropped once it is final" "0 {}" "$(backups) $(state __dbmigrate_backups__)"
expect "said as node says it" 1 \
  "$(grep -c '\[release\] dropping the backups of 20261009000002-m2, it can not be reverted anymore' final.out)"
expect "and its steps recorded as irreversible" \
  '{"t":3,"a":"update","c":["pets"],"n":2},{"t":3,"a":"delete","c":["pets"],"n":3}' \
  "$(state 20261009000002-m2 | grep -o '{"t":3[^}]*}' | tr '\n' ',' | sed 's/,$//')"
run down -c 1 >/dev/null 2>&1
expect "the dml migration can not be reverted any more" 1 \
  "$(run down 2>&1 | grep -q 'can not be reverted' && echo 1)"

project manualbackups
printf '{"deprecation": {"releases": 1}}' >"$dir/.db-migraterc"
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, kind: "string"});' r1
add '  db->insert("pets", [{id: 1, kind: "dog"}]);
  db->update("pets", {kind: "hound"}, {kind: "dog"});' "" dml
add '  db->createTable("a", {id: "int"});' r2
run up >manualb.out 2>&1
expect "manual backups are said to be due" "1 1" \
  "$(backups) $(grep -c 'the backups of 20261009000002-m2 are due for dropping, it ran 1 releases ago. Drop them in a dml migration with db.dropBackups("20261009000002-m2"), it can not be reverted afterwards.' manualb.out)"
add '  db->dropBackups(NULL);' "" dml
run up >manualb2.out 2>&1
expect "until dropped" "0 0" "$(backups) $(grep -c 'backups of .* are due' manualb2.out)"
expect "and then that can not be reverted" 1 \
  "$(run down 2>&1 | grep -q 'can not be reverted' && echo 1)"

project forgets
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, kind: "string"});'
add '  db->insert("pets", [{id: 1, kind: "dog"}]);
  db->update("pets", {kind: "hound"}, {kind: "dog"});' "" dml
run up >/dev/null 2>&1
run down -c 1 >/dev/null 2>&1
expect "reverting forgets the backups" "{}" "$(state __dbmigrate_backups__)"

# ---------------------------------------------------------------- status

project status
echo '{"deprecation": {"releases": 1}}' >"$dir/.db-migraterc"
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}, kind: "string", deleted_at: "datetime"});
  db->createTable("owners", {id: "int"});
  db->deprecateTableWith("owners", {releases: 1});' r1
add '  db->insert("pets", [{id: 1, kind: "dog"}]);
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  db->deleteWith("pets", {id: 1}, {mode: "soft", column: "deleted_at", purge: true});' "" dml
add '  db->createTable("a", {id: "int"});' r2
run up >/dev/null 2>&1
add '  db->createTable("b", {id: "int"});'
expect "status says what is pending, deprecated and due, in node's words" \
'Pending migrations:
  20261009000004-m4
Release: r2
Migration lock: free
Background jobs: none
Deprecated:
  table "owners" renamed to __dbm_deprecated_owners_20261009000001, 1 of 1 releases, due to drop (manual)
Purges:
  "pets" by 20261009000002-m2#3, 1 of 1 releases, due (manual)
Backups:
  20261009000002-m2: 1 table(s), 1 of 1 releases, due (manual)' \
  "$(run status 2>&1 | grep -v '^\[')"

# ------------------------------------------------------- fix and releases

project fix
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  db->createTable("owners", {id: {type: "int", primaryKey: true}});
  db->deprecateTableWith("pets", {releases: 2, drop: "auto"});
  db->deprecateTable("owners");' r1
add '  db->createTable("a", {id: "int"});' r2
add '  db->createTable("b", {id: "int"});' r3
run up >/dev/null 2>&1
expect "renamed and dropped over three releases" "__dbm_deprecated_owners_T a b" "$(tables)"
before=$(state __dbmigrate_schema__)
run fix >fix.out 2>&1
expect "fix learns the steps of the releases again" "$before" "$(state __dbmigrate_schema__)"
expect "said as learning" 1 "$(grep -c '\[release\] learning release r2' fix.out)"
run down -c 2 >/dev/null 2>&1
expect "reverting works on the schema learned again" "owners pets" "$(tables)"
run down >/dev/null 2>&1
expect "all the way" "" "$(tables)"

project once
add '  db->createTable("pets", {id: {type: "int", primaryKey: true}});
  db->addColumn("pets", "name", {type: "string"});'
add '  db->createTable("owners", {id: "int"});'
run up >/dev/null 2>&1
run fix >/dev/null 2>&1
run fix >/dev/null 2>&1
expect "fix learns the records once" 2 \
  "$(state 20261009000001-m1 | grep -o '"a":' | wc -l | tr -d ' ')"
run down -c 2 >/dev/null 2>&1
expect "and down of them empties it" "" "$(tables)"

# ------------------------------------------------------------- refusals

refused() {
  # $1 what, $2 body, $3 message
  rm -rf "$work/refused"
  project refused
  add "$2"
  expect "refuses $1" 1 "$(run up 2>&1 | grep -q -- "$3" && echo 1)"
}
refused "an unknown table" '  db->deprecateTable("nope");' 'The table "nope" is unknown'
refused "an unknown column" '  db->createTable("pets", {id: "int"});
  db->deprecateColumn("pets", "nope");' 'The column "nope" of "pets" is unknown'
refused "dropping what is not deprecated" '  db->createTable("pets", {id: "int"});
  db->dropDeprecatedTable("pets");' 'The table "pets" is not deprecated, deprecate it first'
refused "a drop it does not know" '  db->createTable("pets", {id: "int"});
  db->deprecateTableWith("pets", {drop: "never"});' "drop is 'auto' or 'manual', not never"
refused "less than one release" '  db->createTable("pets", {id: "int"});
  db->deprecateTableWith("pets", {releases: 0});' 'at least 1, not 0'

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
