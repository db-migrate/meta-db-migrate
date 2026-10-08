#!/bin/sh
#
# Builds a program with its migrations compiled in: every source of the app
# and every file under its migrations/ is lowered and linked against the
# library. The result is the one binary that gets deployed.
#
#   ./build-app.sh <app dir> <output> [driver...]
#
# The drivers are the ones the program talks to - pg when none are named -
# and only those are linked, with only the system libraries they need: a
# program on SQLite does not need libpq installed to start.
#
# They go in whole. Drivers and migrations register themselves from
# constructors, and nothing else names them - so a linker left to pick only
# what is referenced would leave every one of them out.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=${META_ROOT:-$here/../../metalanguage}
app=$(CDPATH= cd -- "$1" && pwd)
output=$2
shift 2
drivers=${*:-pg}
work="$here/build/app-$(basename "$app")"
cc=${CC:-cc}
# a migration just created is up and down returning 0, and its db unused
flags="-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -g ${CFLAGS:-}"

"$here/build.sh" >/dev/null

rm -rf "$work"
mkdir -p "$work"

objects=""

# one source, lowered to $2 and compiled beside it
build() {
  source=$1
  lowered=$2

  mkdir -p "$(dirname "$lowered")"

  if ! "$root/meta" -s -emit "$lowered" -I "$here/include" "$source" \
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

  $cc $flags -I "$here/include" -I "$root/runtime/include" \
      -c "$lowered" -o "${lowered%.c}.o"
  objects="$objects ${lowered%.c}.o"
}

for source in "$app"/*.c; do
  [ -f "$source" ] || continue
  build "$source" "$work/app/$(basename "$source")"
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
"$here/build/meta-migrate" embed-sql "$app/migrations" "$work/embedded-sql.c"
$cc $flags -I "$here/include" -I "$root/runtime/include" \
    -c "$work/embedded-sql.c" -o "$work/embedded-sql.o"
objects="$objects $work/embedded-sql.o"

archives=""
libraries=""

for driver in $drivers; do
  if ! grep -q "^$driver " "$here/build/drivers.txt"; then
    echo "there is no $driver driver" >&2
    exit 1
  fi

  archives="$archives $here/build/libdbmigrate-$driver.a"
  libraries="$libraries $(grep "^$driver " "$here/build/drivers.txt" | cut -d' ' -f2-)"
done

$cc $flags -o "$output" $objects \
    -Wl,--whole-archive $archives "$here/build/libdbmigrate.a" \
    -Wl,--no-whole-archive $libraries -lpthread

echo "built $output"
