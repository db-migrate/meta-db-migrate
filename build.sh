#!/bin/sh
#
# Builds everything under build/:
#
#   libdbmigrate.a          the core and the runtime
#   libdbmigrate-core.a     the core alone, for a program that links meta's
#                           runtime itself (lib/libmeta_runtime.a)
#   libdbmigrate-<d>.a      one driver or plugin, for a program that links it
#   libdbmigrate-<d>.so     the same, for the launcher to load
#   meta-migrate            the development launcher
#   drivers.txt             what each driver needs, for build-app.sh
#
# The drivers are whatever is in src/drivers/: pg, cockroachdb (which is pg
# and its own differences), mysql, sqlite3. The plugins are in src/plugins/:
# yaml. Both are built the same way, because both are linked the same way.
#
# Every source is meta, so each one is lowered to C first and the C compiler
# only ever sees the lowered files. What meta knows, meta is asked: where its
# runtime is (`meta -print-config`) and which libraries a driver needs
# (`meta -print-flags`) - through meta's own headers, or as its source names
# them with `#pragma meta needs`. The headers' paths then come from
# pkg-config.
#
#   ./build.sh
#   META_ROOT=/path/to/metalanguage ./build.sh
#   DBM_BUILD_DIR=/elsewhere ./build.sh       instead of build/
#   DBM_STATIC_DEPS=build/deps ./build.sh     the drivers' shared objects
#       carry the libraries tools/deps.sh built inside them, so the launcher
#       needs none of them installed - a release does this
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "${META_ROOT:-$here/../../metalanguage}" && pwd)
meta="$root/meta"
out="${DBM_BUILD_DIR:-$here/build}"
cc=${CC:-cc}
flags="-std=gnu11 -Wall -Wextra -Werror -g ${CFLAGS:-}"

mkdir -p "$out/lowered" "$out/obj"

# one line of `meta -print-config` or `-print-flags`: `key=value`
answer() {
  sed -n "s/^$1=//p"
}

config=$("$meta" -print-config)
runtimeInclude=$(echo "$config" | answer runtime-include)

# ------------------------------------------- what the sources need

# what each driver is made of: cockroachdb is pg and its own differences
sourcesOf() {
  case $1 in
    cockroachdb) echo "$here/src/drivers/pg.c $here/src/drivers/cockroachdb.c" ;;
    yaml) echo "$here/src/plugins/yaml.c" ;;
    *) echo "$here/src/drivers/$1.c" ;;
  esac
}

objectsOf() {
  for source in $(sourcesOf "$1"); do
    echo "$out/obj/driver-$(basename "$source" .c).o"
  done | tr '\n' ' '
}

names=$(for source in "$here"/src/drivers/*.c "$here"/src/plugins/*.c; do
  [ -f "$source" ] && basename "$source" .c
done)

# The libraries a driver needs, as pkg-config names: meta reads all of its
# sources at once - the libraries behind meta's own headers, and those a
# source names with `#pragma meta needs`. Asked before the headers' paths
# are known, so what meta cannot find yet is said on stderr and not wanted.
for driver in $names; do
  echo "$driver $("$meta" -print-flags -I "$here/include" \
                    -I "$here/src/drivers" $(sourcesOf "$driver") 2>/dev/null |
                  answer pkg-config)"
done >"$out/packages.txt"

packagesFor() {
  sed -n "s/^$1 //p" "$out/packages.txt"
}

# the headers' paths: Debian and Ubuntu put libpq-fe.h under
# /usr/include/postgresql, which no compiler looks in
deps=${DBM_STATIC_DEPS:-}
system=""
for package in $(cut -d' ' -f2- "$out/packages.txt" | tr ' ' '\n' | sort -u); do
  system="$system $(pkg-config --cflags-only-I "$package" 2>/dev/null || true)"
done

# the headers of the libraries that will be linked in, before the system's
if [ -n "$deps" ]; then
  deps=$(CDPATH= cd -- "$deps" && pwd)
  system="-I$deps/include $system"
fi

# -------------------------------------------------------------- building

lower() {
  # $1 source, $2 lowered file, the rest handed to meta; a construct meta
  # cannot write back fails it with its reason
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
}

compile() {
  # $1 lowered file, $2 object
  # src/drivers too: a lowered driver no longer sits beside pg_driver.h
  $cc $flags -fPIC -I "$here/include" -I "$here/src/drivers" \
      -I "$runtimeInclude" $system -c "$1" -o "$2"
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

# the runtime, once: as the archive a published meta brings, or from the
# sources of a checkout - meta says which
rm -rf "$out/obj/runtime"
mkdir -p "$out/obj/runtime"
archive=$(echo "$config" | answer runtime-archive)

if [ -n "$archive" ]; then
  (cd "$out/obj/runtime" && ar x "$archive")
else
  for source in $(echo "$config" | answer runtime-sources); do
    $cc -std=gnu11 -O2 -g -fPIC ${CFLAGS:-} -I "$runtimeInclude" \
        -c "$source" -o "$out/obj/runtime/$(basename "$source" .c).o"
  done
fi

runtime=$(ls "$out"/obj/runtime/*.o)

rm -f "$out"/libdbmigrate*.a
ar rcs "$out/libdbmigrate.a" $core $runtime
ar rcs "$out/libdbmigrate-core.a" $core

# ---------------------------------------------------------------- linking

# one library, linked dynamically: what pkg-config says, or the package's
# own name where pkg-config does not know it (libpq -lpq, yaml-0.1 -lyaml)
dynamicLibraryOf() {
  pkg-config --libs-only-l "$1" 2>/dev/null ||
    echo "-l$(echo "$1" | sed 's/^lib//; s/-[0-9.]*$//')"
}

dynamicLibrariesOf() {
  for package in $(packagesFor "$1"); do
    dynamicLibraryOf "$package"
  done | tr '\n' ' '
}

# The libraries that go into a shared object with DBM_STATIC_DEPS: those
# tools/deps.sh built - it writes a .pc for each - statically and hidden, so
# they meet nothing else the launcher loads; the rest, mysql among them, as
# they are. glibc's own stay dynamic, always.
sharedLibrariesOf() {

  static=""
  dynamic=""

  for package in $(packagesFor "$1"); do
    if [ -n "$deps" ] && [ -f "$deps/lib/pkgconfig/$package.pc" ]; then
      static="$static $package"
    else
      dynamic="$dynamic $(dynamicLibraryOf "$package")"
    fi
  done

  if [ -n "$static" ]; then
    linked=$(PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig" \
             pkg-config --static --libs $static)
    glibc=$(echo "$linked" | tr ' ' '\n' | grep -xE -- '-l(pthread|dl|m|rt|c)' | tr '\n' ' ')
    others=$(echo "$linked" | tr ' ' '\n' | grep -vxE -- '-l(pthread|dl|m|rt|c)' | tr '\n' ' ')
    echo "-Wl,--exclude-libs,ALL -Wl,-Bstatic $others -Wl,-Bdynamic $glibc $dynamic"
  else
    echo "$dynamic"
  fi
}

# each driver twice: an archive for a program that links it, and a shared
# object for the launcher, whose core symbols come from the launcher itself
for driver in $names; do
  ar rcs "$out/libdbmigrate-$driver.a" $(objectsOf $driver)
  $cc $flags -shared -o "$out/libdbmigrate-$driver.so" $(objectsOf $driver) \
      $(sharedLibrariesOf $driver)
done

# for build-app.sh: each driver, its pkg-config names, its libraries
for driver in $names; do
  echo "$driver|$(packagesFor $driver)|$(dynamicLibrariesOf $driver)"
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
