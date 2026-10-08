#!/bin/sh
#
# The plugins: database.yml, a project's own plugins in plugins/ - in the
# launcher and compiled into a program - and the ssh tunnel.
#
#   test/plugins.sh
#
# SQLite for most of it. The tunnel goes to the PostgreSQL test/run.sh uses,
# through an sshd started here on 127.0.0.1:52222 as this user, with keys of
# its own; without sshd that part is skipped.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
work="$top/build/test-plugins"
meta="$top/build/meta-migrate"
failed=0

export PGPASSWORD=dbm

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
mkdir -p "$work/yaml/migrations" "$work/own/plugins" "$work/own/migrations"

migration() {
  cat >"$1/migrations/20261008200000-t.c" <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("t", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("t"); }
DBM_MIGRATION(up, down)
EOF
  printf '#include <db_migrate.h>\nint main(int c, char **v) { return dbmCli(c, v); }\n' \
    >"$1/main.c"
}

tables() {
  sqlite3 "$1" "select coalesce(group_concat(name, ','), '') from (select
    name from sqlite_master where type = 'table' and name not like 'sqlite_%'
    order by name)" 2>&1
}

# ------------------------------------------------------------ database.yml

cd "$work/yaml"
migration .
cat >database.yml <<'EOF'
defaults: &defaults
  driver: sqlite3
  filename: wrong.db
dev:
  <<: *defaults
  filename: {ENV: DBM_TEST_FILE}
quoted:
  <<: *defaults
  filename: "123"
EOF

DBM_TEST_FILE=right.db "$meta" up >/dev/null 2>&1
expect "database.yml is the configuration without database.json" \
  "migrations,migrations_state,t" "$(tables right.db)"
expect "a merged key gives way to one written beside it" "" \
  "$(ls wrong.db 2>/dev/null)"
"$meta" up -e quoted >/dev/null 2>&1
expect "a quoted number stays text" "migrations,migrations_state,t" \
  "$(tables 123)"
printf 'dev: [unclosed\n' >bad.yml
expect "YAML that does not parse says where" \
  "[ERROR] bad.yml is not YAML: did not find expected ',' or ']' at line 2" \
  "$("$meta" up --config bad.yml 2>&1)"

"$top/build-app.sh" . ./with-yaml sqlite3 yaml >/dev/null 2>&1
rm -f right.db
DBM_TEST_FILE=right.db ./with-yaml up >/dev/null 2>&1
expect "a program with the yaml plugin linked reads it too" \
  "migrations,migrations_state,t" "$(tables right.db)"
"$top/build-app.sh" . ./without-yaml sqlite3 >/dev/null 2>&1
expect "one without it says what is missing" 1 \
  "$(./without-yaml up 2>&1 | grep -c 'nothing in this program reads .yml files')"
expect "and only one with it needs libyaml" "1 0" \
  "$(ldd ./with-yaml | grep -c libyaml) $(ldd ./without-yaml | grep -c libyaml)"

# ---------------------------------------------------- a project's plugins

cd "$work/own"
migration .
cat >plugins/house.c <<'EOF'
#include <db_migrate_plugin.h>

#include <stdio.h>

/** Every migration of ours starts with its table. */
static int house(const char *dir, const char *stamp, const char *title) {

  char path[512];

  dbmWrite(path, sizeof path, TEXT`${dir}/${stamp}-${title}.c`);

  FILE *file = fopen(path, "wx");

  if (file == NULL)
    return 1;

  dbmSay(file, TEXT`#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("${title}", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("${title}"); }
DBM_MIGRATION(up, down)
`);
  fclose(file);
  return 0;
}

/** `<env> <driver> <file>`, a line each. */
static bool conf(const char *file, json_t *config, char *why, size_t room) {

  FILE *in = fopen(file, "r");
  char env[64], driver[64], name[256];

  if (in == NULL) {
    dbmWrite(why, room, TEXT`cannot read ${file}`);
    return false;
  }

  dbm_text_t json = {0};
  defer json.release();
  json.put("{");

  for (int n = 0; fscanf(in, "%63s %63s %255s", env, driver, name) == 3; ++n)
    json.append(TEXT`${n > 0 ? "," : ""}"${env}": {"driver": "${driver}", "filename": "${name}"}`);

  json.put("}");
  fclose(in);
  *config = meta_toJSON(json.text);
  return true;
}

__attribute__((constructor)) static void registerHouse(void) {
  dbmRegisterTemplate("house", house);
  dbmRegisterConfigLoader(".conf", conf);
}
EOF
echo "dev sqlite3 house.db" >database.conf

"$meta" create pets --template house >/dev/null 2>&1
"$meta" up >/dev/null 2>&1
expect "the launcher opens plugins/ - its loader and its template" \
  "migrations,migrations_state,pets,t" "$(tables house.db)"
expect "a template nobody registered is refused" 1 \
  "$("$meta" create x --template nope 2>&1 | grep -c 'there is no template called nope')"
"$meta" create kennels --v2-file >/dev/null 2>&1
expect "--v2-file writes a v2 migration" 1 \
  "$(grep -c 'DBM_MIGRATION_V2(migrate)' migrations/*-kennels.c)"

"$top/build-app.sh" . ./app sqlite3 >/dev/null 2>&1
rm -f house.db
./app up >/dev/null 2>&1
expect "and build-app.sh compiles them in" \
  "migrations,migrations_state,pets,t" "$(tables house.db)"
./app create cats --template house >/dev/null 2>&1
expect "the template too" 1 "$(ls migrations/*-cats.c 2>/dev/null | wc -l)"

# ------------------------------------------------------------- ssh tunnel

sshd=$(command -v sshd || echo /usr/sbin/sshd)

if [ ! -x "$sshd" ] ||
   ! psql -h 127.0.0.1 -p 55432 -U postgres -c '' >/dev/null 2>&1; then
  echo "skip     the ssh tunnel: no sshd, or no PostgreSQL on 55432"
else
  mkdir -p "$work/ssh/migrations"
  cd "$work/ssh"
  migration .

  ssh-keygen -q -t ed25519 -N '' -f hostkey
  ssh-keygen -q -t ed25519 -N '' -f plain
  ssh-keygen -q -t ed25519 -N 'geheim' -f locked
  cat plain.pub locked.pub >authorized_keys
  echo "[127.0.0.1]:52222 $(cut -d' ' -f1,2 hostkey.pub)" >known_hosts

  cat >sshd_config <<EOF
Port 52222
ListenAddress 127.0.0.1
HostKey $PWD/hostkey
AuthorizedKeysFile $PWD/authorized_keys
PidFile $PWD/sshd.pid
UsePAM no
StrictModes no
PasswordAuthentication no
AllowTcpForwarding yes
EOF
  "$sshd" -f "$PWD/sshd_config" -E "$PWD/sshd.log"
  sleep 0.5

  psql -h 127.0.0.1 -p 55432 -U postgres -q \
    -c "drop database if exists tunnel" -c "create database tunnel" \
    >/dev/null 2>&1

  # the tunnel in YAML, as node's plugin-tunnel-ssh had it in JSON
  cat >database.yml <<EOF
base: &base
  driver: pg
  host: 127.0.0.1
  port: 55432
  user: postgres
  password: dbm
  database: tunnel
ssh: &ssh
  host: 127.0.0.1
  port: 52222
  username: $(id -un)
  options: {UserKnownHostsFile: $PWD/known_hosts, StrictHostKeyChecking: "yes"}
dev:
  <<: *base
  tunnel: {<<: *ssh, privateKeyPath: $PWD/plain}
locked:
  <<: *base
  tunnel: {<<: *ssh, privateKeyPath: $PWD/locked, passphrase: geheim}
refused:
  <<: *base
  tunnel: {<<: *ssh, privateKeyPath: $PWD/missing}
EOF

  "$meta" up >up.out 2>&1
  expect "a migration through the tunnel" \
    "migrations,migrations_state,t" \
    "$(psql -h 127.0.0.1 -p 55432 -U postgres tunnel -tAq -c \
      "select string_agg(tablename, ',' order by tablename) from pg_tables
       where schemaname = 'public'")"
  "$meta" down -e locked >/dev/null 2>&1
  expect "and back, with a key that has a passphrase" "" \
    "$(psql -h 127.0.0.1 -p 55432 -U postgres tunnel -tAq -c \
      "select tablename from pg_tables where tablename = 't'")"
  expect "both through sshd" 2 "$(grep -c 'Accepted publickey' sshd.log)"
  expect "a tunnel that does not open says what ssh said" 1 \
    "$("$meta" up -e refused 2>&1 | tr '\n' ' ' | grep -c 'the ssh tunnel did not open: .*Permission denied')"
  expect "and no ssh is left behind" 0 \
    "$(ps -eo args | grep -c '^ssh -N.*52222')"

  kill "$(cat sshd.pid)"
fi

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
