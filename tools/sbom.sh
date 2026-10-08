#!/bin/sh
#
# What a release is made of, as CycloneDX, and the license texts that have
# to travel with it.
#
#   tools/sbom.sh <version> <meta> <deps> <sbom.json> <notices>
#   tools/sbom.sh 0.0.1 /opt/meta build/deps dist/sbom.cdx.json THIRD_PARTY_NOTICES
#
# Only what we put in ourselves, which no scanner finds in a binary:
#
#   meta's runtime    in libdbmigrate.a, and so in every program built with
#                     it - proprietary, from the image wxone/meta
#   yyjson            the same way, MIT
#   lib/deps/*        what build-app.sh --static links in (tools/deps.sh)
#
# glibc is the host's and is not in it. META_IMAGE and META_DIGEST say which
# meta it was, when the release was built from the image.
set -eu

version=$1
meta=$2
deps=$3
sbom=$4
notices=$5

yyjson=$(sed -n 's/^#define YYJSON_VERSION_STRING "\(.*\)"/\1/p' \
  "$meta/runtime/vendor/yyjson/yyjson.h")

python3 - "$version" "$yyjson" "$deps/deps.json" "$sbom" \
  "${META_IMAGE:-}" "${META_DIGEST:-}" <<'EOF'
import json, sys, uuid, datetime

version, yyjson, depsFile, out, image, digest = sys.argv[1:7]
deps = json.load(open(depsFile))

def component(name, version, license, purl, description, **extra):
    c = {"type": "library", "bom-ref": purl, "name": name,
         "version": version, "purl": purl, "description": description}
    c["licenses"] = ([{"license": {"id": license}}] if license != "proprietary"
                     else [{"license": {"name": "proprietary"}}])
    c.update(extra)
    return c

components = [
    component("meta-runtime", digest or "unknown", "proprietary",
              "pkg:docker/%s%s" % (image or "wxone/meta",
                                   "@" + digest if digest else ""),
              "meta's runtime (task scheduler), statically in libdbmigrate.a"),
    component("yyjson", yyjson, "MIT", "pkg:github/ibireme/yyjson@" + yyjson,
              "JSON library, statically in libdbmigrate.a"),
]

for d in deps:
    extra = {}
    if "sha256" in d:
        extra["hashes"] = [{"alg": "SHA-256", "content": d["sha256"]}]
        extra["externalReferences"] = [{"type": "distribution",
                                        "url": d["source"]}]
    if "built" in d:
        extra["properties"] = [{"name": "configure", "value": d["built"]}]
    components.append(component(
        d["name"], d["version"], d["license"], d["purl"],
        "statically in programs built with build-app.sh --static; "
        + d["source"], **extra))

root = "pkg:github/db-migrate/meta-db-migrate@v" + version
bom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.6",
    "serialNumber": "urn:uuid:" + str(uuid.uuid4()),
    "version": 1,
    "metadata": {
        "timestamp": datetime.datetime.now(datetime.timezone.utc)
                         .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "component": {"type": "application", "bom-ref": root,
                      "name": "meta-db-migrate", "version": version,
                      "purl": root,
                      "licenses": [{"license": {"id": "MIT"}}]},
    },
    "components": components,
    "dependencies": [{"ref": root,
                      "dependsOn": [c["bom-ref"] for c in components]}],
}
json.dump(bom, open(out, "w"), indent=2)
EOF

{
  cat <<EOF
meta-db-migrate $version - third-party notices

meta-db-migrate itself is MIT licensed, see LICENSE. These are the parts of
others that a release carries: in libdbmigrate.a, and so in every program
built with it, and in lib/deps, which build-app.sh --static links in.

glibc is not among them: it stays dynamic and is the host's.

================================================================ meta runtime

The runtime of the meta language (task scheduler), in libdbmigrate.a.
Proprietary.

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
