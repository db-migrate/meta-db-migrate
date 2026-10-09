#!/bin/sh
#
# Builds a program with its migrations compiled in: every source of the app
# and every file under its migrations/ is lowered and linked against the
# library. The result is the one binary that gets deployed.
#
#   ./build-app.sh [--static] <app dir> <output> [driver or plugin...]
#
# The drivers are the ones the program talks to - pg when none are named -
# and only those are linked, with only the system libraries they need: a
# program on SQLite does not need libpq installed to start. The shipped
# plugins are named the same way (`pg yaml`), and the app's own, in its
# plugins/ directory, are always compiled in.
#
# --static links those libraries into the program as well - libpq, OpenSSL,
# SQLite, libyaml, from tools/deps.sh - so that on the host it needs glibc
# and nothing else. glibc itself stays dynamic: name resolution goes through
# its NSS modules, which are the host's. libmysqlclient stays dynamic too,
# for its license (GPL-2.0 with the FOSS exception).
#
# They go in whole. Drivers, plugins and migrations register themselves from
# constructors, and nothing else names them - so a linker left to pick only
# what is referenced would leave every one of them out.
#
# It runs from a checkout, where it builds the library first, and from a
# release, where it is bin/meta-migrate-build-app and the library is built.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

staticBuild=false
if [ "${1:-}" = "--static" ]; then
  staticBuild=true
  shift
fi

app=$(CDPATH= cd -- "$1" && pwd)
output=$2
shift 2
drivers=${*:-pg}
cc=${CC:-cc}
# a migration just created is up and down returning 0, and its db unused
flags="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g ${CFLAGS:-}"

if [ -f "$here/build.sh" ]; then
  # a checkout
  root=${META_ROOT:-$here/../../metalanguage}
  include="$here/include"
  lib=${DBM_BUILD_DIR:-$here/build}
  launcher="$lib/meta-migrate"
  deps=${DBM_DEPS:-$lib/deps}
  work="$lib/app-$(basename "$app")"
  "$here/build.sh" >/dev/null
else
  # a release: <prefix>/bin/meta-migrate-build-app
  prefix=$(dirname "$here")
  root=${META_ROOT:-/opt/meta}
  include="$prefix/include"
  lib="$prefix/lib"
  launcher="$prefix/bin/meta-migrate"
  deps=${DBM_DEPS:-$prefix/lib/deps}
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
fi

if $staticBuild && [ ! -f "$deps/deps.json" ]; then
  echo "--static needs the libraries tools/deps.sh builds, in $deps (DBM_DEPS)" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"

objects=""

# where this meta keeps its runtime's headers
runtimeInclude=$("$root/meta" -print-config | sed -n 's/^runtime-include=//p')

# one source, lowered to $2 and compiled beside it
build() {
  source=$1
  lowered=$2

  mkdir -p "$(dirname "$lowered")"

  # a construct meta cannot write back fails it, with its reason
  if ! "$root/meta" -s -emit "$lowered" -I "$include" "$source" \
      >"$lowered.log" 2>&1; then
    echo "lowering $source failed:" >&2
    cat "$lowered.log" >&2
    exit 1
  fi

  $cc $flags -I "$include" -I "$runtimeInclude" \
      -c "$lowered" -o "${lowered%.c}.o"
  objects="$objects ${lowered%.c}.o"
}

for source in "$app"/*.c; do
  [ -f "$source" ] || continue
  build "$source" "$work/app/$(basename "$source")"
done

for source in "$app"/plugins/*.c; do
  [ -f "$source" ] || continue
  build "$source" "$work/plugins/$(basename "$source")"
done

# a migration is lowered under migrations/[scope/]<its own name>, because the
# C compiler expands __FILE__ to the path it is given and DBM_MIGRATION reads
# the name - and the scope - off it
for source in "$app"/migrations/*.c "$app"/migrations/*/*.c; do
  [ -f "$source" ] || continue
  build "$source" "$work/${source#"$app"/}"
done

# migrations written as SQL go in as C string literals, so the program still
# needs nothing beside it
"$launcher" embed-sql "$app/migrations" "$work/embedded-sql.c"
$cc $flags -I "$include" -I "$runtimeInclude" \
    -c "$work/embedded-sql.c" -o "$work/embedded-sql.o"
objects="$objects $work/embedded-sql.o"

# drivers.txt says, per driver: its libraries as pkg-config names, and as
# linker flags. With --static, those tools/deps.sh built - it wrote a .pc
# for each - go into the program; glibc's own and the rest stay dynamic.
archives=""
libraries=""
static=""

for driver in $drivers; do
  line=$(grep "^$driver|" "$lib/drivers.txt" || true)

  if [ -z "$line" ]; then
    echo "there is no $driver driver or plugin" >&2
    exit 1
  fi

  archives="$archives $lib/libdbmigrate-$driver.a"
  packages=$(echo "$line" | cut -d'|' -f2)
  dynamic=$(echo "$line" | cut -d'|' -f3)
  linkedIn=""

  for package in $packages; do
    if $staticBuild && [ -f "$deps/lib/pkgconfig/$package.pc" ]; then
      linkedIn="$linkedIn $package"
    fi
  done

  # all of the driver's libraries in, or the driver as the system has it
  if [ -n "$linkedIn" ] && [ "$(echo $linkedIn)" = "$(echo $packages)" ]; then
    static="$static $linkedIn"
  else
    libraries="$libraries $dynamic"
  fi
done

if [ -n "$static" ]; then
  flags_=$(PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig" pkg-config --static --libs $static)
  glibc=$(echo "$flags_" | tr ' ' '\n' | grep -xE -- '-l(pthread|dl|m|rt|c)' | tr '\n' ' ')
  others=$(echo "$flags_" | tr ' ' '\n' | grep -vxE -- '-l(pthread|dl|m|rt|c)' | tr '\n' ' ')
  libraries="-Wl,-Bstatic $others -Wl,-Bdynamic $glibc $libraries"
fi

$cc $flags -o "$output" $objects \
    -Wl,--whole-archive $archives "$lib/libdbmigrate.a" \
    -Wl,--no-whole-archive $libraries -lpthread -ldl -lm

echo "built $output"
