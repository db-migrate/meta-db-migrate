#!/bin/sh
#
# A release: the launcher, the library and its drivers and plugins, the
# headers, build-app and the static libraries it links in with --static,
# built against the meta the metalanguage repository publishes - with the
# license, the third-party notices and a CycloneDX SBOM.
#
#   tools/release.sh <meta> <prefix> <tarball>
#   tools/release.sh /opt/meta /opt/meta-db-migrate dist/x.tar.gz
#
# The static libraries are built by tools/deps.sh, into DBM_DEPS if that is
# set and has them already. The SBOM also lands beside the tarball, as
# <tarball without .tar.gz>.cdx.json.
#
# meta is not in it. It comes from the image wxone/meta, whose /opt/meta is
# where the launcher looks for it - and meta has to stay there, since it
# bakes that path in. The tarball unpacks at /:
#
#   tar -xzf x.tar.gz -C /
#
#   <prefix>/bin/meta-migrate
#   <prefix>/bin/meta-migrate-build-app
#   <prefix>/include/            db_migrate.h and the rest
#   <prefix>/lib/                libdbmigrate.a, libdbmigrate-<d>.{a,so}
#   <prefix>/lib/deps/           libpq, OpenSSL, SQLite, libyaml, static
#   <prefix>/share/doc/meta-db-migrate/
#                                LICENSE, THIRD_PARTY_NOTICES, sbom.cdx.json
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

# emptied, not removed: under /opt it is made by root and handed over, and
# whoever releases may not make it again
mkdir -p "$prefix"
find "$prefix" -mindepth 1 -delete
mkdir -p "$prefix/bin" "$prefix/include" "$prefix/lib"

# built apart from build/, which stays the development build
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
META_ROOT="$meta" DBM_BUILD_DIR="$build" "$here/build.sh"

cp "$build/meta-migrate" "$prefix/bin/"
cp "$here/build-app.sh" "$prefix/bin/meta-migrate-build-app"
cp -r "$here/include/." "$prefix/include/"
cp "$build"/libdbmigrate*.a "$build"/libdbmigrate-*.so "$build/drivers.txt" \
   "$prefix/lib/"

deps=${DBM_DEPS:-$build/deps}
[ -f "$deps/deps.json" ] || "$here/tools/deps.sh" "$deps"
mkdir -p "$prefix/lib/deps"
cp -r "$deps/lib" "$deps/include" "$deps/deps.json" "$prefix/lib/deps/"

version=$(sed -n 's/^#define DBM_VERSION "\(.*\)"/\1/p' "$here/include/db_migrate.h")
doc="$prefix/share/doc/meta-db-migrate"
mkdir -p "$doc"
cp "$here/LICENSE" "$here/README.md" "$doc/"
"$here/tools/sbom.sh" "$version" "$meta" "$deps" "$doc/sbom.cdx.json" \
  "$doc/THIRD_PARTY_NOTICES" >/dev/null

mkdir -p "$(dirname "$tarball")"
tar -czf "$tarball" -C / "${prefix#/}"
cp "$doc/sbom.cdx.json" "${tarball%.tar.gz}.cdx.json"
echo "released $tarball"
