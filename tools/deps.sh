#!/bin/sh
#
# The client libraries a program built with `build-app.sh --static` carries
# inside itself, so that on the host it needs nothing but glibc.
#
#   tools/deps.sh <dir>
#
# libpq is built from PostgreSQL's source, without GSSAPI and LDAP: the
# distribution's libpq.a wants Kerberos, which no distribution ships static.
# That needs a C compiler, curl, bzip2, bison, flex and perl.
# OpenSSL, SQLite and libyaml are the distribution's static archives.
#
# glibc stays dynamic, as everywhere: name resolution goes through its NSS
# modules, which are the host's. libmysqlclient is left out on purpose - it
# is GPL-2.0 with the FOSS exception, and inside a program that is not free
# software that is not ours to hand out; mysql is linked dynamically.
#
# <dir>/lib          the archives
# <dir>/include      libpq's headers
# <dir>/deps.json    each one: name, version, license, where it came from -
#                    what tools/sbom.sh and tools/notices.sh read
# <dir>/licenses/    their license texts
set -eu

pg_version=18.6
pg_sha256=555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f

out=$1
mkdir -p "$out/lib" "$out/include" "$out/licenses"
out=$(CDPATH= cd -- "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
multiarch=$(cc -print-multiarch)
system=/usr/lib/$multiarch

# --------------------------------------------------------------- libpq

tarball="$work/postgresql-$pg_version.tar.bz2"
curl -fsSL -o "$tarball" \
  "https://ftp.postgresql.org/pub/source/v$pg_version/postgresql-$pg_version.tar.bz2"
echo "$pg_sha256  $tarball" | sha256sum -c - >/dev/null

tar -xjf "$tarball" -C "$work"
(
  cd "$work/postgresql-$pg_version"
  # nothing libpq does not need: no Kerberos, LDAP, ICU, readline, zlib
  ./configure --quiet --with-ssl=openssl --without-gssapi --without-ldap \
    --without-icu --without-readline --without-zlib CFLAGS="-O2 -fPIC" \
    >/dev/null
  make -s -C src/include >/dev/null
  make -s -C src/common >/dev/null
  make -s -C src/port >/dev/null
  make -s -C src/interfaces/libpq all-static-lib >/dev/null
)

pg="$work/postgresql-$pg_version"
cp "$pg/src/interfaces/libpq/libpq.a" "$out/lib/"
cp "$pg/src/common/libpgcommon_shlib.a" "$out/lib/libpgcommon.a"
cp "$pg/src/port/libpgport_shlib.a" "$out/lib/libpgport.a"
cp "$pg/src/interfaces/libpq/libpq-fe.h" "$pg/src/include/postgres_ext.h" \
   "$out/include/"
# before PostgreSQL 18, postgres_ext.h included it
[ ! -f "$pg/src/include/pg_config_ext.h" ] ||
  cp "$pg/src/include/pg_config_ext.h" "$out/include/"
cp "$pg/COPYRIGHT" "$out/licenses/postgresql.txt"

if nm -u "$out/lib/libpq.a" | grep -q " U gss_\| U ldap_"; then
  echo "libpq still wants GSSAPI or LDAP" >&2
  exit 1
fi

# ------------------------------------------- the distribution's archives

# package, the archives it brings, the name it goes by, its license
packaged() {
  package=$1
  name=$2
  license=$3
  shift 3

  for archive in "$@"; do
    cp "$system/$archive" "$out/lib/"
  done

  version=$(dpkg-query -W -f '${Version}' "$package")
  cp "/usr/share/doc/$package/copyright" "$out/licenses/$name.txt"
  . /etc/os-release
  printf '{"name": "%s", "version": "%s", "license": "%s", "purl": "pkg:deb/%s/%s@%s?arch=%s&distro=%s-%s", "source": "%s %s package"},\n' \
    "$name" "$version" "$license" "$ID" "$package" "$version" \
    "$(dpkg --print-architecture)" "$ID" "$VERSION_ID" "$ID $VERSION_ID" \
    "$package" >>"$work/packaged.json"
}

: >"$work/packaged.json"
packaged libssl-dev openssl Apache-2.0 libssl.a libcrypto.a
packaged libsqlite3-dev sqlite blessing libsqlite3.a
packaged libyaml-dev libyaml MIT libyaml.a

cat >"$out/deps.json" <<EOF
[
{"name": "libpq", "version": "$pg_version", "license": "PostgreSQL", "purl": "pkg:generic/postgresql@$pg_version", "source": "https://ftp.postgresql.org/pub/source/v$pg_version/postgresql-$pg_version.tar.bz2", "sha256": "$pg_sha256", "built": "--with-ssl=openssl --without-gssapi --without-ldap --without-icu --without-readline --without-zlib"},
$(sed '$ s/,$//' "$work/packaged.json")
]
EOF

python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$out/deps.json"
echo "deps in $out: $(ls "$out/lib" | tr '\n' ' ')"
