#!/bin/sh
#
# Builds everything under build/:
#
#   libdbmigrate.a          the core and the runtime
#   libdbmigrate-<d>.a      one driver or plugin, for a program that links it
#   libdbmigrate-<d>.so     the same, for the launcher to load
#   meta-migrate            the development launcher
#
# The drivers are whatever is in src/drivers/: pg, cockroachdb (which is pg
# and its own differences), mysql, sqlite3. The plugins are in src/plugins/:
# yaml. Both are built the same way, because both are linked the same way.
#
# Every source is meta, so each one is lowered to C first and the C compiler
# only ever sees the lowered files. The runtime is the two objects meta's own
# programs link: the task scheduler and yyjson.
#
#   ./build.sh
#   META_ROOT=/path/to/metalanguage ./build.sh
#   DBM_BUILD_DIR=/elsewhere ./build.sh       instead of build/
#   DBM_STATIC_DEPS=build/deps ./build.sh     the drivers' shared objects
#       carry libpq, OpenSSL, SQLite and libyaml (tools/deps.sh) inside them,
#       so the launcher needs none of them installed - a release does this
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "${META_ROOT:-$here/../../metalanguage}" && pwd)
meta="$root/meta"
out="${DBM_BUILD_DIR:-$here/build}"
cc=${CC:-cc}
flags="-std=gnu11 -Wall -Wextra -Werror -g ${CFLAGS:-}"

mkdir -p "$out/lowered" "$out/obj"

# where the system keeps the client libraries' headers: Debian and Ubuntu put
# libpq-fe.h under /usr/include/postgresql, which no compiler looks in
system=""
for package in libpq mysqlclient libmariadb sqlite3 yaml-0.1; do
  system="$system $(pkg-config --cflags-only-I "$package" 2>/dev/null || true)"
done

# the headers of the libraries that will be linked in, before the system's
deps=${DBM_STATIC_DEPS:-}
if [ -n "$deps" ]; then
  deps=$(CDPATH= cd -- "$deps" && pwd)
  system="-I$deps/include $system"
fi

lower() {
  # $1 source, $2 lowered file, the rest handed to meta
  source=$1
  lowered=$2
  shift 2

  # $system unquoted: it is a list of -I flags, or nothing
  if ! "$meta" -s -emit "$lowered" -I "$here/include" $system "$@" "$source" \
      >"$lowered.log" 2>&1; then
    echo "lowering $source failed:" >&2
    cat "$lowered.log" >&2
    exit 1
  fi

  if grep -q "^#error meta cannot write back" "$lowered"; then
    echo "meta could not write back part of $source:" >&2
    grep "^#error" "$lowered" >&2
    exit 1
  fi
}

compile() {
  # $1 lowered file, $2 object
  # src/drivers too: a lowered driver no longer sits beside pg_driver.h
  $cc $flags -fPIC -I "$here/include" -I "$here/src/drivers" \
      -I "$root/runtime/include" $system -c "$1" -o "$2"
}

core=""
for source in "$here"/src/*.c; do
  name=$(basename "$source" .c)
  lower "$source" "$out/lowered/$name.c"
  compile "$out/lowered/$name.c" "$out/obj/$name.o"
  core="$core $out/obj/$name.o"
done

drivers=""
for source in "$here"/src/drivers/*.c "$here"/src/plugins/*.c; do
  [ -f "$source" ] || continue
  name=$(basename "$source" .c)
  lower "$source" "$out/lowered/driver-$name.c"
  compile "$out/lowered/driver-$name.c" "$out/obj/driver-$name.o"
  drivers="$drivers $out/obj/driver-$name.o"
done

# the runtime, once: from its sources in a metalanguage checkout, or as the
# objects a published meta brings in lib/libmeta_runtime.a
if [ -f "$root/runtime/meta_tasks.c" ]; then
  $cc -std=gnu11 -O2 -g -fPIC ${CFLAGS:-} -I "$root/runtime/include" \
      -c "$root/runtime/meta_tasks.c" -o "$out/obj/meta_tasks.o"
  $cc -std=gnu11 -O2 -g -fPIC ${CFLAGS:-} \
      -c "$root/runtime/vendor/yyjson/yyjson.c" -o "$out/obj/yyjson.o"
  runtime="$out/obj/meta_tasks.o $out/obj/yyjson.o"
elif [ -f "$root/lib/libmeta_runtime.a" ]; then
  rm -rf "$out/obj/runtime"
  mkdir -p "$out/obj/runtime"
  (cd "$out/obj/runtime" && ar x "$root/lib/libmeta_runtime.a")
  runtime=$(ls "$out"/obj/runtime/*.o)
else
  echo "$root has neither runtime/meta_tasks.c nor lib/libmeta_runtime.a" >&2
  exit 1
fi

rm -f "$out"/libdbmigrate*.a
ar rcs "$out/libdbmigrate.a" $core $runtime

# what each driver is made of, and what it needs from the system
objectsOf() {
  case $1 in
    cockroachdb) echo "$out/obj/driver-pg.o $out/obj/driver-cockroachdb.o" ;;
    *) echo "$out/obj/driver-$1.o" ;;
  esac
}

librariesOf() {
  case $1 in
    pg|cockroachdb) echo "-lpq" ;;
    mysql) echo "-lmysqlclient" ;;
    sqlite3) echo "-lsqlite3" ;;
    yaml) echo "-lyaml -lm" ;;
  esac
}

# what a driver's shared object carries inside it, with DBM_STATIC_DEPS -
# hidden, so they meet nothing else the launcher loads; mysql stays dynamic,
# for libmysqlclient's GPL
sharedLibrariesOf() {
  static=""
  case $1 in
    pg|cockroachdb) static="-lpq -lpgcommon -lpgport -lssl -lcrypto" ;;
    sqlite3) static="-lsqlite3" ;;
    yaml) static="-lyaml" ;;
  esac

  if [ -n "$deps" ] && [ -n "$static" ]; then
    echo "-L$deps/lib -Wl,--exclude-libs,ALL -Wl,-Bstatic $static -Wl,-Bdynamic -lpthread -ldl -lm"
  else
    librariesOf "$1"
  fi
}

# each driver twice: an archive for a program that links it, and a shared
# object for the launcher, whose core symbols come from the launcher itself
names=$(for object in $drivers; do
  basename "$object" .o | sed 's/^driver-//'
done)

for driver in $names; do
  ar rcs "$out/libdbmigrate-$driver.a" $(objectsOf $driver)
  $cc $flags -shared -o "$out/libdbmigrate-$driver.so" $(objectsOf $driver) \
      $(sharedLibrariesOf $driver)
done

# for build-app.sh, so it does not have to know the table above
for driver in $names; do
  echo "$driver $(librariesOf $driver)"
done >"$out/drivers.txt"

# the launcher: the core and the runtime, exported for the migrations and
# the drivers it opens, and no driver of its own
if [ -f "$here/tools/meta-migrate.c" ]; then

  lower "$here/tools/meta-migrate.c" "$out/lowered/meta-migrate.c" \
      -D "DBM_META_ROOT=\"$root\"" \
      -D "DBM_INCLUDE_DIR=\"$here/include\"" \
      -D "DBM_DRIVER_DIR=\"$out\""
  compile "$out/lowered/meta-migrate.c" "$out/obj/meta-migrate.o"
  $cc $flags -rdynamic -o "$out/meta-migrate" "$out/obj/meta-migrate.o" \
      $core $runtime -ldl -lpthread

fi

echo "built $out: libdbmigrate.a, drivers and plugins:" $names
