#!/bin/sh
#
# Background migrations as node 1.4 to 1.6 have them: a dml migration with
# { background: true } registered as a job by up, run by `work` - stopped
# and continued, blocking the ones after it, taken over from a worker that
# died, paused while migrations run, reverted by down.
#
#   test/work.sh
#
# SQLite, so it needs nothing running. node's test/work_test.js.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-work"
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
JOB=20261009000003-m3

lite() {
  sqlite3 -cmd '.timeout 5000' "$dir/db" "$1" 2>&1
}

pets() {
  lite "select coalesce(group_concat(row, ' '), '') from (select id || '|' ||
    coalesce(name, '') || '|' || coalesce(kind, '') as row from pets order by id)"
}

kinds() {
  lite "select group_concat(kind, ' ') from (select kind from pets order by id)"
}

jobs() {
  lite "select coalesce((select json_extract(value, '\$.jobs') from migrations_state
    where key = '__dbmigrate_jobs__'), '{}')"
}

job() {
  # $1 the field of the job
  lite "select json_extract(value, '\$.jobs.\"$JOB\".$1') from migrations_state
    where key = '__dbmigrate_jobs__'"
}

recorded() {
  lite "select count(*) from migrations"
}

run() {
  (cd "$dir" && "$meta" "$@" --lock-timeout 2000 --lock-interval 100)
}

# project <name> <body>...: m1 the table, m2 the data, then each body - a dml
# migration, `background` in its _meta when the body asks for it
project() {
  dir="$work/$1"
  shift
  mkdir -p "$dir/migrations"
  echo '{"dev": {"driver": "sqlite3", "filename": "db"}}' >"$dir/database.json"
  cat >"$dir/migrations/20261009000001-m1.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("pets", {
    id: {type: "int", primaryKey: true},
    name: {type: "string"}, kind: {type: "string"}, deleted_at: {type: "datetime"},
  });
  return db->createTable("runs", {name: {type: "string"}, what: {type: "string"}, at: {type: "real"}});
}
DBM_MIGRATION_V2(migrate)
EOF
  cat >"$dir/migrations/20261009000002-m2.c" <<'EOF'
#include <db_migrate.h>
static int migrate(dml_t *db) {
  return db->insertColumns("pets", ["id", "name", "kind"], [ [1, "Rex", "dog"],
    [2, "Tom", "cat"], [3, "Nemo", "fish"], [4, "Bello", "dog"],
    [5, "Lassie", "dog"], [6, "Dory", "fish"] ]);
}
DBM_MIGRATION_DML(migrate)
EOF
  i=3
  for body in "$@"; do
    extra=$(echo "$body" | sed -n 's|^ */\* meta: \(.*\) \*/$|\1|p')
    {
      echo '#include <db_migrate.h>'
      echo '#include <stdlib.h>'
      echo '#include <unistd.h>'
      echo 'static int migrate(dml_t *db) {'
      echo "$body"
      echo '  return db->hasFailed() ? -1 : 0;'
      echo '}'
      if [ -n "$extra" ]; then
        echo "DBM_MIGRATION_DML_WITH(migrate, $extra)"
      else
        echo 'DBM_MIGRATION_DML(migrate)'
      fi
    } >"$dir/migrations/2026100900000$i-m$i.c"
    i=$((i + 1))
  done
}

BACKGROUND='  /* meta: {background: true} */
  db->updateWith("pets", {kind: "hound"}, {kind: "dog"}, {batch: 1});
  db->deleteWith("pets", {kind: "fish"}, {mode: "soft", column: "deleted_at", batch: 1});'

# a row into runs, at the time it is written in ms
logged() {
  echo "  db->runSql(\"INSERT INTO runs (name, what, at) VALUES ('$1', '$2', (julianday('now') - 2440587.5) * 86400000)\", {irreversible: true});"
}

rm -rf "$work"
mkdir -p "$work"
cd "$work"

# ---------------------------------------------------- run as jobs by work

project jobs "$BACKGROUND" '  db->insert("pets", {id: 7, name: "Tim", kind: "cat"});'
run up >up.out 2>&1
expect "up registers it and runs the ones after it" \
  "/20261009000001-m1 /20261009000002-m2 /20261009000004-m4" \
  "$(lite "select group_concat(name, ' ') from (select name from migrations order by name)")"
expect "as a queued job, in node's words" "queued 1" \
  "$(job s) $(grep -c "\[jobs\] $JOB runs in the background" up.out)"
expect "the job as node writes it" \
  '{"step":0,"learned":0,"done":0,"rb":0,"s":"queued","blocking":false,"ID":0}' \
  "$(lite "select json_extract(value, '\$.jobs.\"$JOB\"') from migrations_state where key = '__dbmigrate_jobs__'" | sed 's/,"n":"[0-9a-f]*"//')"
run up >up2.out 2>&1
expect "up again neither runs nor registers it again" "1 $JOB" \
  "$(grep -c "$JOB is running in the background" up2.out) $(lite "select group_concat(key) from json_each((select json_extract(value, '\$.jobs') from migrations_state where key = '__dbmigrate_jobs__'))")"
run work --pause 1 --interval 10 >work.out 2>&1
expect "work runs it" "{} 4" "$(jobs) $(recorded)"
expect "and says so" "1 1" \
  "$(grep -c "\[jobs\] $JOB is done" work.out) $(grep -c '\[jobs\] 1 job(s) done' work.out)"
expect "what it did" "3 6" \
  "$(lite "select group_concat(id, ' ') from (select id from pets where deleted_at is not null order by id)")"
run down -c 2 >/dev/null 2>&1
expect "and down reverts it as any migration" "$ORIGINAL" "$(pets)"

# ------------------------------------------------------ stop and continue

project stop "$BACKGROUND"
run up >/dev/null 2>&1
(cd "$dir" && exec "$meta" work --pause 300 --batch 1 --interval 10 >stop.out 2>&1) &
worker=$!
sleep 0.5
kill -TERM $worker
wait $worker
expect "a stopped job is queued again with its progress" "queued 1" "$(job s) $(job step)"
expect "said as node says it" 1 \
  "$(grep -c "\[jobs\] $JOB stopped, it continues with the next run" "$dir/stop.out")"
run work --interval 10 >continue.out 2>&1
expect "the next work continues it" "1 hound cat fish hound hound fish" \
  "$(grep -c "\[jobs\] continuing $JOB" continue.out) $(kinds)"

# ------------------------------------------------------------- blocking

timed() {
  # $1 id, $2 name, $3 ms to take, $4 more _meta
  echo "  /* meta: {background: true${4:-}} */"
  logged "$2" start
  echo "  db->update(\"pets\", {name: \"$2\"}, {id: $1});"
  echo "  usleep($3 * 1000);"
  logged "$2" end
}
project blocking "$(timed 1 a 400 ', blocking: true')" "$(timed 2 b 0)" "$(timed 3 c 0)"
run up >/dev/null 2>&1
run work --parallel 3 --interval 10 >blocking.out 2>&1
expect "three jobs on three slots" "{} 3" "$(jobs) $(grep -c 'is done' blocking.out)"
expect "the ones after a blocking job wait until it is done" "1 1" \
  "$(lite "select (select at from runs where name = 'b' and what = 'start') >= (select at from runs where name = 'a' and what = 'end')") $(lite "select (select at from runs where name = 'c' and what = 'start') >= (select at from runs where name = 'a' and what = 'end')")"

# ------------------------------------------------- each job runs once

once() {
  echo '  /* meta: {background: true} */'
  logged "$1" ran
  echo "  db->update(\"pets\", {name: \"x$1\"}, {id: $1});"
}
project once "$(once 1)" "$(once 2)" "$(once 3)" "$(once 4)"
run up >/dev/null 2>&1
(cd "$dir" && "$meta" work --parallel 2 --interval 10 >w1.out 2>&1) &
first=$!
(cd "$dir" && "$meta" work --parallel 2 --interval 10 >w2.out 2>&1) &
second=$!
wait $first $second
expect "several workers run each job once" "1:1 2:1 3:1 4:1 {}" \
  "$(lite "select group_concat(name || ':' || n, ' ') from (select name, count(*) n from runs group by name order by name)") $(jobs)"

# ------------------------------------------------------------ failed jobs

project failing '  /* meta: {background: true} */
  db->update("pets", {kind: "hound"}, {kind: "dog"});
  if (getenv("DBM_TEST_FIXED") == NULL)
    return db->fail(TEXT`broken`);'
run up >/dev/null 2>&1
run work --interval 10 >failing.out 2>&1
expect "a failed job is rolled back and says why" "failed broken 0 $ORIGINAL" \
  "$(job s) $(job err) $(job step) $(pets)"
expect "work fails with it" 1 \
  "$(grep -c "1 background job(s) failed: $JOB" failing.out)"
run work --interval 10 >again.out 2>&1
expect "and does not take it again" 1 "$(grep -c '0 job(s) done' again.out)"
DBM_TEST_FIXED=1 run up >/dev/null 2>&1
DBM_TEST_FIXED=1 run work --interval 10 >fixed.out 2>&1
expect "until up queues it again" "{} hound cat fish hound hound fish" "$(jobs) $(kinds)"

# ------------------------------------------------------------- takeover

project takeover "$BACKGROUND"
run up >/dev/null 2>&1
lite "update migrations_state set value = json_set(value,
  '\$.jobs.\"$JOB\".s', 'running', '\$.jobs.\"$JOB\".ID', 'dead',
  '\$.jobs.\"$JOB\".step', 1) where key = '__dbmigrate_jobs__'"
(cd "$dir" && exec "$meta" work --watch --job-timeout 300 --interval 50 >takeover.out 2>&1) &
worker=$!
tries=0
while [ "$(recorded)" -lt 3 ] && [ $tries -lt 100 ]; do sleep 0.1; tries=$((tries + 1)); done
kill -TERM $worker
wait $worker
expect "a job whose worker died is taken over" "1 hound cat fish hound hound fish" \
  "$(grep -c "taking over $JOB, its worker did not respond for 300 ms" "$dir/takeover.out") $(kinds)"

# --------------------------------------------- paused while migrations run

project pause "$BACKGROUND"
run up >/dev/null 2>&1
cat >"$dir/migrations/20261009000004-m4.c" <<EOF
#include <db_migrate.h>
#include <unistd.h>
static int up(migrator_t *db) {
  db->runSql("CREATE TABLE before AS SELECT value FROM migrations_state WHERE key = '$JOB'");
  db->runSql("CREATE TABLE paused AS SELECT value FROM migrations_state WHERE key = '__dbmigrate_jobs__'");
  usleep(500000);
  return db->runSql("CREATE TABLE after AS SELECT value FROM migrations_state WHERE key = '$JOB'");
}
static int down(migrator_t *db) { (void)db; return 0; }
DBM_MIGRATION(up, down)
EOF
# compiled now, so the up below pauses the job at once, not after compiling
run up --dry-run >/dev/null 2>&1
(cd "$dir" && exec "$meta" work --pause 500 --batch 1 --watch --interval 20 >pause.out 2>&1) &
worker=$!
tries=0
while [ "$(lite "select json_array_length(value, '\$.s') from migrations_state where key = '$JOB'")" = 0 ] &&
  [ $tries -lt 100 ]; do sleep 0.02; tries=$((tries + 1)); done
run up >pauseup.out 2>&1
tries=0
while [ "$(recorded)" -lt 4 ] && [ $tries -lt 200 ]; do sleep 0.05; tries=$((tries + 1)); done
kill -TERM $worker
wait $worker
expect "a migration pauses the job: its progress stays while it runs" 1 \
  "$(lite "select (select value from before) = (select value from after)")"
expect "the pause is there meanwhile, and gone afterwards" "1 0" \
  "$(lite "select json_type(value, '\$.pause') is not null from paused") $(lite "select json_type(value, '\$.pause') is not null from migrations_state where key = '__dbmigrate_jobs__'")"
expect "the job pauses and goes on" "1 hound cat fish hound hound fish" \
  "$(grep -c "\[jobs\] $JOB pauses for migrations" "$dir/pause.out") $(kinds)"

# a pause whose holder is gone does not hold
project stale "$BACKGROUND"
run up >/dev/null 2>&1
lite "update migrations_state set value = json_set(value, '\$.pause', json('{\"ID\":\"gone\",\"n\":\"x\"}'))
  where key = '__dbmigrate_jobs__'"
run work --interval 10 >stale.out 2>&1
expect "a pause whose holder let go does not hold" 1 "$(grep -c "$JOB is done" stale.out)"

# ---------------------------------------------------- down reverts jobs

project down "$BACKGROUND"
run up >/dev/null 2>&1
(cd "$dir" && exec "$meta" work --pause 300 --batch 1 --interval 10 >/dev/null 2>&1) &
worker=$!
sleep 0.5
kill -TERM $worker
wait $worker
run down >down.out 2>&1
expect "down reverts a job stopped half way first" \
  "{} $ORIGINAL 2 0" \
  "$(jobs) $(pets) $(recorded) $(lite "select count(*) from sqlite_master where name like '__dbm_backup%'")"
run up >/dev/null 2>&1
run down >/dev/null 2>&1
expect "and one not started yet" "{} 2" "$(jobs) $(recorded)"
run down >/dev/null 2>&1
expect "then the migrations" "" "$(pets)"

# ------------------------------------------------ background needs dml

dir="$work/notdml"
mkdir -p "$dir/migrations"
echo '{"dev": {"driver": "sqlite3", "filename": "db"}}' >"$dir/database.json"
cat >"$dir/migrations/20261009000001-m1.c" <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) { return db->createTable("pets", {id: {type: "int"}}); }
DBM_MIGRATION_V2_WITH(migrate, {background: true})
EOF
expect "background needs a dml migration" 1 \
  "$(run up 2>&1 | grep -c 'runs in the background, which only dml migrations can')"

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
