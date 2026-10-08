#!/bin/sh
#
# A release: the launcher, the library and its drivers and plugins, and the
# headers, built against the meta the metalanguage repository publishes.
#
#   tools/release.sh <meta> <prefix> <tarball>
#   tools/release.sh /opt/meta /opt/meta-db-migrate dist/x.tar.gz
#
# meta is not in it. It comes from the image wxone/meta, whose /opt/meta is
# where the launcher looks for it - and meta has to stay there, since it
# bakes that path in. The tarball unpacks at /:
#
#   tar -xzf x.tar.gz -C /
#
#   <prefix>/bin/meta-migrate
#   <prefix>/include/            db_migrate.h and the rest
#   <prefix>/lib/                libdbmigrate.a, libdbmigrate-<d>.{a,so}
#
# db-migrate's own parts are also found beside the launcher wherever it is.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
meta=$1
prefix=$2
tarball=$3

for dir in "$meta" "$prefix"; do
  case $dir in
    /*) ;;
    *) echo "$dir is not absolute - the release unpacks at /" >&2; exit 2 ;;
  esac
done

if [ ! -x "$meta/meta" ]; then
  echo "there is no meta in $meta" >&2
  exit 1
fi

rm -rf "$prefix"
mkdir -p "$prefix/bin" "$prefix/include" "$prefix/lib"

# built apart from build/, which stays the development build
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
META_ROOT="$meta" DBM_BUILD_DIR="$build" "$here/build.sh"

cp "$build/meta-migrate" "$prefix/bin/"
cp -r "$here/include/." "$prefix/include/"
cp "$build"/libdbmigrate*.a "$build"/libdbmigrate-*.so "$build/drivers.txt" \
   "$prefix/lib/"

mkdir -p "$(dirname "$tarball")"
tar -czf "$tarball" -C / "${prefix#/}"
echo "released $tarball"
