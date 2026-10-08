#!/bin/sh
#
# A release: the launcher, the library and its drivers and plugins, the
# headers, and the meta that compiles migrations - as one tarball that
# unpacks to its prefix.
#
#   tools/release.sh <metalanguage checkout> <prefix> <tarball>
#   tools/release.sh ../../metalanguage /opt/meta-db-migrate dist/x.tar.gz
#
# meta bakes where its runtime headers are into itself when it is built, so
# it is built where it will be: the checkout is copied to <prefix>/lib/meta
# and built there. That makes <prefix> the one place the release works from.
# db-migrate's own parts are found beside the launcher wherever it is.
#
#   <prefix>/bin/meta-migrate
#   <prefix>/include/            db_migrate.h and the rest
#   <prefix>/lib/                libdbmigrate.a, libdbmigrate-<d>.{a,so}
#   <prefix>/lib/meta/           meta, runtime/include
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source=$(CDPATH= cd -- "$1" && pwd)
prefix=$2
tarball=$3

case $prefix in
  /*) ;;
  *) echo "the prefix is absolute: /opt/meta-db-migrate" >&2; exit 2 ;;
esac

rm -rf "$prefix"
mkdir -p "$prefix/bin" "$prefix/include" "$prefix/lib/meta"

# meta, built in place: what it bakes in is where it stays
(cd "$source" && git archive --format=tar HEAD) | tar -x -C "$prefix/lib/meta"
(cd "$prefix/lib/meta" && cmake . >/dev/null && make -j"$(nproc)" meta >/dev/null)

# built apart from build/, which stays the development build
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
META_ROOT="$prefix/lib/meta" DBM_BUILD_DIR="$build" "$here/build.sh"

cp "$build/meta-migrate" "$prefix/bin/"
cp -r "$here/include/." "$prefix/include/"
cp "$build"/libdbmigrate*.a "$build"/libdbmigrate-*.so "$build/drivers.txt" \
   "$prefix/lib/"

# of meta, only what compiling a migration needs
keep="$prefix/lib/meta.keep"
mkdir -p "$keep/runtime"
cp "$prefix/lib/meta/meta" "$prefix/lib/meta/builtinDefines.h" "$keep/"
cp -r "$prefix/lib/meta/runtime/include" "$prefix/lib/meta/runtime/vendor" \
   "$keep/runtime/"
rm -rf "$prefix/lib/meta"
mv "$keep" "$prefix/lib/meta"

mkdir -p "$(dirname "$tarball")"
tar -czf "$tarball" -C "$(dirname "$prefix")" "$(basename "$prefix")"
echo "released $tarball"
