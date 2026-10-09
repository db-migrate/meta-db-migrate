#!/bin/sh
#
# Migrating from inside a program that links meta's runtime itself - an
# nginx module, say: a shared object with libdbmigrate-core.a, a driver,
# its migrations and lib/libmeta_runtime.a, which calls dbmMigrateUp when
# it starts and logs through its own logger.
#
#   test/embed.sh
#
# SQLite, so it needs nothing running. The migrations are a v1 and a v2
# one, since v2 is what needs the state and the lock dbmMigrateUp opens.
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
top="$here/.."
root=$(CDPATH= cd -- "${META_ROOT:-$top/../../metalanguage}" && pwd)
work="$top/build/test-embed"
cc=${CC:-cc}
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

rm -rf "$work"
mkdir -p "$work/migrations" "$work/lowered/migrations"
cd "$work"

cat >module.c <<'EOF'
#include <db_migrate.h>

#include <stdio.h>

static void toLog(int level, const char *line) {
  fprintf(stderr, "module log %d: %s\n", level, line);
}

/** What the module does when it starts. */
int moduleStart(const char *file) {

  char why[512];
  json_t config = meta_toJSON("{\"driver\": \"sqlite3\"}");
  defer config.releaseAt();

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = yyjson_val_mut_copy(doc, config.node);
  yyjson_mut_obj_add_str(doc, root, "filename", file);
  yyjson_mut_doc_set_root(doc, root);

  json_t withFile = meta_jsonFromMut(doc);
  defer withFile.releaseAt();

  dbmSetLogger(toLog);

  if (dbmMigrateUp(withFile, NULL, why, sizeof why) != 0) {
    fprintf(stderr, "module failed: %s\n", why);
    return 1;
  }

  return 0;
}
EOF

cat >migrations/20261008160000-owners.c <<'EOF'
#include <db_migrate.h>
static int migrate(schema_t *db) {
  db->createTable("owners", {id: {type: "int", primaryKey: true}, name: "string"});
  return db->addColumn("owners", "email", {type: "string", length: 120});
}
DBM_MIGRATION_V2(migrate)
EOF

cat >migrations/20261008170000-pets.c <<'EOF'
#include <db_migrate.h>
static int up(migrator_t *db) {
  return db->createTable("pets", {id: {type: "int", primaryKey: true}});
}
static int down(migrator_t *db) { return db->dropTable("pets"); }
DBM_MIGRATION(up, down)
EOF

cat >host.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {

  void *module = dlopen(argv[1], RTLD_NOW);
  int (*start)(const char *);

  if (module == NULL) {
    fprintf(stderr, "%s\n", dlerror());
    return 2;
  }

  start = (int (*)(const char *))dlsym(module, "moduleStart");
  return start(argv[2]);
}
EOF

config=$("$root/meta" -print-config)
runtimeInclude=$(echo "$config" | sed -n 's/^runtime-include=//p')

# lowered as build-app.sh does: a migration under migrations/, for __FILE__
objects=""
for source in module.c migrations/*.c; do
  lowered="lowered/$source"
  "$root/meta" -s -emit "$lowered" -I "$top/include" "$source" \
    >"$lowered.log" 2>&1 || { cat "$lowered.log"; exit 1; }
  $cc -std=gnu11 -fPIC -g -I "$top/include" -I "$runtimeInclude" \
    -c "$lowered" -o "${lowered%.c}.o" || exit 1
  objects="$objects ${lowered%.c}.o"
done

# meta's runtime as the program brings it, apart from libdbmigrate-core.a:
# the archive a published meta has, or one made of a checkout's objects
runtime=$(echo "$config" | sed -n 's/^runtime-archive=//p')
if [ -z "$runtime" ]; then
  ar rcs libmeta_runtime.a "$top"/build/obj/runtime/*.o
  runtime="$work/libmeta_runtime.a"
fi

$cc -shared -o module.so $objects \
  -Wl,--whole-archive "$top/build/libdbmigrate-sqlite3.a" \
  "$top/build/libdbmigrate-core.a" -Wl,--no-whole-archive \
  "$runtime" -lsqlite3 -lpthread -ldl -lm 2>link.out
expect "core, a driver and meta's runtime link into one shared object" "" \
  "$(cat link.out)"

$cc -o host host.c -ldl

./host ./module.so "$work/embed.db" >first.out 2>&1
expect "the module migrates when it starts" 0 "$?"
expect "both migrations, the v2 one with node's state" \
  "migrations,migrations_state,owners,pets" \
  "$(sqlite3 embed.db "select group_concat(name, ',') from (select name from
     sqlite_master where type = 'table' and name not like 'sqlite_%'
     order by name)")"
expect "and its lock released" '"ID":0' \
  "$(sqlite3 embed.db "select value from migrations_state where key =
     '__dbmigrate_state__'" | grep -o '"ID":0')"
expect "what it says goes to the module's log" 1 \
  "$(grep -c '^module log 1: \[INFO\] Done' first.out)"

./host ./module.so "$work/embed.db" >second.out 2>&1
expect "started again, there is nothing to do" "0 1" \
  "$? $(grep -c 'No migrations to run' second.out)"

# pets forgotten, so up makes a table that is there
sqlite3 embed.db "delete from migrations where name like '%pets'"
./host ./module.so "$work/embed.db" >third.out 2>&1
expect "a failure comes back with its reason" 1 \
  "$(grep -c '^module failed: .' third.out)"

cd "$here"
echo "$failed failed"
[ "$failed" -eq 0 ]
