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
flags="-std=gnu11 -Wall -Wextra -Werror -g ${CFLAGS:-}"

"$here/build.sh" >/dev/null

rm -rf "$work"
mkdir -p "$work"

objects=""

for source in "$app"/*.c "$app"/migrations/*.c; do
  [ -f "$source" ] || continue

  name=$(basename "$source" .c)
  lowered="$work/$name.c"

  # lowered under the name it was written as, so __FILE__ is the migration's
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
      -c "$lowered" -o "$work/$name.o"
  objects="$objects $work/$name.o"
done

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
