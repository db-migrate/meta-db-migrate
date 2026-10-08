#!/bin/sh
#
# What a release is made of, as CycloneDX, and the license texts that have
# to travel with it.
#
#   tools/sbom.sh <version> <meta> <deps> <sbom.json> <notices>
#   tools/sbom.sh 0.1.0 /opt/meta build/deps dist/sbom.cdx.json THIRD_PARTY_NOTICES
#
# Only what we put in ourselves, which no scanner finds in a binary:
#
#   meta              the compiler, in the release at /opt/meta - proprietary,
#                     from the image wxone/meta
#   meta's runtime    in libdbmigrate.a, and so in every program built with
#                     it - the same
#   yyjson            the same way, MIT
#   lib/deps/*        what build-app.sh --static links in (tools/deps.sh)
#
# meta is proprietary today and is meant to become open source. The license
# fields say what holds now; the intention is an annotation and a property
# beside them, which bind nobody to anything and grant nothing.
#
# glibc is the host's and is listed as excluded, with GLIBC_MIN. META_IMAGE
# and META_DIGEST say which meta it was; STAGE, the installed release, adds
# its files by their hashes. The document itself is tools/sbom.py's, in the
# shape of wx1-keyagent's ci/sbom.sh.
set -eu

version=$1
meta=$2
deps=$3
sbom=$4
notices=$5

yyjson=$(sed -n 's/^#define YYJSON_VERSION_STRING "\(.*\)"/\1/p' \
  "$meta/runtime/vendor/yyjson/yyjson.h")

python3 "$(dirname "$0")/sbom.py" "$version" "$yyjson" "$deps/deps.json" \
  "$sbom" "${META_IMAGE:-}" "${META_DIGEST:-}" "${GLIBC_MIN:-}" \
  "$(git -C "$(dirname "$0")" rev-parse HEAD 2>/dev/null || echo unknown)" \
  "$meta" "${STAGE:-}"

{
  cat <<EOF
meta-db-migrate $version - third-party notices

meta-db-migrate itself is MIT licensed, see LICENSE. These are the parts of
others that a release carries: meta, in /opt/meta; in libdbmigrate.a, and so
in every program built with it; and in lib/deps, which build-app.sh --static
links in.

glibc is not among them: it stays dynamic and is the host's.

======================================================== meta and its runtime

The meta compiler, at /opt/meta, and its runtime (task scheduler), in
libdbmigrate.a and so in every program built with it. Proprietary.

meta and its runtime are meant to be released under an open-source license
in the future. Until then the proprietary license applies.

====================================================================== yyjson

EOF
  # the license is the header's first comment
  sed -n '1,/\*\//p' "$meta/runtime/vendor/yyjson/yyjson.h"

  for license in "$deps"/licenses/*.txt; do
    name=$(basename "$license" .txt)
    printf '\n%s %s\n\n' "$(printf '=%.0s' $(seq 1 $((76 - ${#name}))))" "$name"
    cat "$license"
  done
} >"$notices"

echo "sbom $sbom, notices $notices"
