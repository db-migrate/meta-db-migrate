"""
The CycloneDX document for tools/sbom.sh, in the shape wx1-keyagent's
ci/sbom.sh writes, so that one can be nested in the other.

  sbom.py <version> <yyjson> <deps.json> <out> <image> <digest> <glibc>
          <commit> <meta dir> <stage>

<stage>, the installed release, adds the files shipped by their hashes; empty
leaves them out.
"""
import datetime
import hashlib
import json
import os
import sys
import uuid
from urllib.parse import quote

(version, yyjson, depsFile, out, image, digest, glibc, commit, metaDir,
 stage) = sys.argv[1:11]
deps = json.load(open(depsFile))
now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def generic(name, version, url=""):
    purl = "pkg:generic/%s@%s" % (name, version)
    return purl + ("?download_url=" + quote(url, safe="") if url else "")


def licenses(license):
    if any(word in license for word in (" WITH ", " AND ", " OR ")):
        return [{"expression": license}]
    if license.startswith("proprietary"):
        return [{"license": {"name": license}}]
    return [{"license": {"id": license}}]


def library(name, version, license, ref, description, purl=None, **extra):
    c = {"type": "library", "bom-ref": ref, "name": name, "version": version,
         "licenses": licenses(license), "scope": "required",
         "description": description}
    if purl:
        c["purl"] = purl
    c.update(extra)
    return c


# meta: proprietary today, meant to become open source - the license field
# says the first, a property and an annotation the second
meta = image.split(":")[0] if image else "wxone/meta"
fromMeta = {
    "externalReferences": [{"type": "distribution",
                            "url": "https://hub.docker.com/r/" + meta}],
    "properties": [
        {"name": "wxone:license:planned", "value": "open-source"},
        {"name": "wxone:image",
         "value": (image or meta) + ("@" + digest if digest else "")}],
}

components = [
    library("meta", digest or "unknown", "proprietary (wx-one)",
            "pkg:generic/meta",
            "the meta compiler, shipped at /opt/meta; the launcher and "
            "build-app.sh lower migrations and programs with it",
            type="application", **fromMeta),
    library("meta-runtime", digest or "unknown", "proprietary (wx-one)",
            "pkg:generic/meta-runtime",
            "runtime of the meta compiler (libmeta_runtime.a and its headers), "
            "in libdbmigrate.a and so in every program built with it",
            **fromMeta),
    library("yyjson", yyjson, "MIT", generic("yyjson", yyjson),
            "vendored in the meta runtime, linked statically",
            purl=generic("yyjson", yyjson)),
]

for d in deps:
    extra = {}
    if "sha256" in d:
        purl = generic(d["name"], d["version"], d["source"])
        extra["hashes"] = [{"alg": "SHA-256", "content": d["sha256"]}]
        extra["externalReferences"] = [{"type": "distribution",
                                        "url": d["source"]}]
        how = "built from source by tools/deps.sh (%s)" % d["built"]
    else:
        purl = d["purl"]
        how = "the " + d["source"] + "'s static archive"
    components.append(library(
        d["name"], d["version"], d["license"], purl,
        how + "; in the launcher's drivers and in programs built with "
        "build-app.sh --static", purl=purl, **extra))

components.append({
    "type": "library", "bom-ref": "pkg:generic/glibc", "name": "glibc",
    "version": ">= " + glibc if glibc else "unknown",
    "licenses": [{"license": {"id": "LGPL-2.1-or-later"}}],
    "scope": "excluded",
    "description": "provided by the host, linked dynamically (libc, libm, "
                   "ld-linux); not part of the release"})

# the files shipped, by their hashes - not the headers
if stage:
    for top in (stage, metaDir):
        for directory, _, files in sorted(os.walk(top)):
            for name in sorted(files):
                path = os.path.join(directory, name)
                if "/include/" in path:
                    continue
                shown = os.path.relpath(path, "/")
                with open(path, "rb") as f:
                    sha = hashlib.sha256(f.read()).hexdigest()
                components.append({
                    "type": "file", "bom-ref": "file:" + shown, "name": shown,
                    "hashes": [{"alg": "SHA-256", "content": sha}]})

root = "pkg:github/db-migrate/meta-db-migrate@v" + version
bom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.6",
    "serialNumber": "urn:uuid:" + str(uuid.uuid4()),
    "version": 1,
    "metadata": {
        "timestamp": now,
        "tools": {"components": [{"type": "application",
                                  "name": "meta-db-migrate tools/sbom.sh"}]},
        "component": {
            "type": "application", "bom-ref": root, "name": "meta-db-migrate",
            "version": version, "purl": root,
            "licenses": [{"license": {"id": "MIT"}}],
            "externalReferences": [{
                "type": "vcs",
                "url": "https://github.com/db-migrate/meta-db-migrate"}],
            "properties": [{"name": "git:commit", "value": commit}]},
    },
    "components": components,
    "dependencies": [{"ref": root, "dependsOn": [
        c["bom-ref"] for c in components if c["type"] != "file"]}],
    "annotations": [{
        "subjects": ["pkg:generic/meta", "pkg:generic/meta-runtime"],
        "annotator": {"organization": {"name": "wx-one"}},
        "timestamp": now,
        "text": "meta and its runtime are meant to be released under an "
                "open-source license in the future. Until then the "
                "proprietary license stated here applies.",
    }],
}

with open(out, "w") as f:
    json.dump(bom, f, indent=2)
