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

static=false
if [ "${1:-}" = "--static" ]; then
  static=true
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

if $static && [ ! -f "$deps/deps.json" ]; then
  echo "--static needs the libraries tools/deps.sh builds, in $deps (DBM_DEPS)" >&2
  exit 1
fi

rm -rf "$work"
mkdir -p "$work"

objects=""

# one source, lowered to $2 and compiled beside it
build() {
  source=$1
  lowered=$2

  mkdir -p "$(dirname "$lowered")"

  if ! "$root/meta" -s -emit "$lowered" -I "$include" "$source" \
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

  $cc $flags -I "$include" -I "$root/runtime/include" \
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
$cc $flags -I "$include" -I "$root/runtime/include" \
    -c "$work/embedded-sql.c" -o "$work/embedded-sql.o"
objects="$objects $work/embedded-sql.o"

# what a driver needs, linked into the program
staticOf() {
  case $1 in
    pg|cockroachdb) echo "-lpq -lpgcommon -lpgport -lssl -lcrypto" ;;
    sqlite3) echo "-lsqlite3" ;;
    yaml) echo "-lyaml" ;;
  esac
}

archives=""
libraries=""
linkedIn=""

for driver in $drivers; do
  if ! grep -q "^$driver " "$lib/drivers.txt"; then
    echo "there is no $driver driver or plugin" >&2
    exit 1
  fi

  archives="$archives $lib/libdbmigrate-$driver.a"

  if $static && [ -n "$(staticOf "$driver")" ]; then
    linkedIn="$linkedIn $(staticOf "$driver")"
  else
    libraries="$libraries $(grep "^$driver " "$lib/drivers.txt" | cut -d' ' -f2-)"
  fi
done

if [ -n "$linkedIn" ]; then
  linkedIn="-L$deps/lib -Wl,-Bstatic $linkedIn -Wl,-Bdynamic"
fi

$cc $flags -o "$output" $objects \
    -Wl,--whole-archive $archives "$lib/libdbmigrate.a" \
    -Wl,--no-whole-archive $linkedIn $libraries -lpthread -ldl -lm

echo "built $output"
