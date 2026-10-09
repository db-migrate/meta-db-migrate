#!/bin/sh
#
# The client libraries a program built with `build-app.sh --static` carries
# inside itself, so that on the host it needs nothing but glibc.
#
#   tools/deps.sh <dir>
#
# libpq is built from PostgreSQL's source, without GSSAPI and LDAP: the
# distribution's libpq.a wants Kerberos, which no distribution ships static.
# That needs a C compiler, curl, bzip2, bison, flex and perl. SQLite is built
# from its amalgamation, since the distribution's libsqlite3.a cannot go into
# a shared object. OpenSSL and libyaml are the distribution's archives. All
# of them are position independent, so the launcher's drivers can carry them
# as well as programs can.
#
# glibc stays dynamic, as everywhere: name resolution goes through its NSS
# modules, which are the host's. libmysqlclient is left out on purpose - it
# is GPL-2.0 with the FOSS exception, and inside a program that is not free
# software that is not ours to hand out; mysql is linked dynamically.
#
# <dir>/lib          the archives, and lib/pkgconfig a .pc for each
# <dir>/include      libpq's headers
# <dir>/deps.json    each one: name, version, license, where it came from -
#                    what tools/sbom.sh and tools/notices.sh read
# <dir>/licenses/    their license texts
set -eu

pg_version=18.6
pg_sha256=555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f

# 3.53.4; sqlite.org publishes SHA3-256 628a44cf...227934e for it
sqlite_version=3.53.4
sqlite_file=sqlite-amalgamation-3530400
sqlite_year=2026
sqlite_sha256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d

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

# -------------------------------------------------------------- SQLite

zip="$work/$sqlite_file.zip"
curl -fsSL -o "$zip" "https://www.sqlite.org/$sqlite_year/$sqlite_file.zip"
echo "$sqlite_sha256  $zip" | sha256sum -c - >/dev/null
python3 -m zipfile -e "$zip" "$work"

cc -O2 -fPIC -DSQLITE_THREADSAFE=1 -c "$work/$sqlite_file/sqlite3.c" \
   -o "$work/sqlite3.o"
ar rcs "$out/lib/libsqlite3.a" "$work/sqlite3.o"
cp "$work/$sqlite_file/sqlite3.h" "$out/include/"
# SQLite is in the public domain; its blessing is the header's first comment
sed -n '1,/\*\*\*\*\*\*\*\*\*\*\*\*\*\*/p' "$work/$sqlite_file/sqlite3.h" \
  >"$out/licenses/sqlite.txt"

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
packaged libyaml-dev libyaml MIT libyaml.a

cat >"$out/deps.json" <<EOF
[
{"name": "libpq", "version": "$pg_version", "license": "PostgreSQL", "purl": "pkg:generic/postgresql@$pg_version", "source": "https://ftp.postgresql.org/pub/source/v$pg_version/postgresql-$pg_version.tar.bz2", "sha256": "$pg_sha256", "built": "--with-ssl=openssl --without-gssapi --without-ldap --without-icu --without-readline --without-zlib"},
{"name": "sqlite", "version": "$sqlite_version", "license": "blessing", "purl": "pkg:generic/sqlite@$sqlite_version", "source": "https://www.sqlite.org/$sqlite_year/$sqlite_file.zip", "sha256": "$sqlite_sha256", "built": "-O2 -fPIC -DSQLITE_THREADSAFE=1"},
$(sed '$ s/,$//' "$work/packaged.json")
]
EOF

python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$out/deps.json"

# -------------------------------------------------- how to link them

# A .pc for each, so build.sh and build-app.sh ask pkg-config --static
# for what a library pulls in - libpq OpenSSL, SQLite libm - rather than
# knowing it. Relative to where they lie, so an unpacked release has them
# right too.
mkdir -p "$out/lib/pkgconfig"

pc() {
  # $1 name, $2 version, $3 -l flag, $4 Libs.private, $5 Requires.private
  cat >"$out/lib/pkgconfig/$1.pc" <<EOF
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: $1
Description: $1, built by meta-db-migrate's tools/deps.sh
Version: $2
Libs: -L\${libdir} $3
Libs.private: $4
Requires.private: $5
Cflags: -I\${includedir}
EOF
}

openssl=$(dpkg-query -W -f '${Version}' libssl-dev | sed 's/-.*//')
yaml=$(dpkg-query -W -f '${Version}' libyaml-dev | sed 's/-.*//')

pc libpq "$pg_version" -lpq "-lpgcommon -lpgport" "libssl libcrypto"
pc libssl "$openssl" -lssl "" "libcrypto"
pc libcrypto "$openssl" -lcrypto "-ldl -lpthread" ""
pc sqlite3 "$sqlite_version" -lsqlite3 "-lm -ldl -lpthread" ""
pc yaml-0.1 "$yaml" -lyaml "" ""
echo "deps in $out: $(ls "$out/lib" | tr '\n' ' ')"
