#!/usr/bin/env bash
# Builds the OpenSSL that fastmm-live wheels link: a pinned release checked against its SHA-256,
# static position-independent libraries and headers only, installed to PREFIX.
# CIBW_BEFORE_ALL in .github/workflows/wheels.yml runs it.
#
#   scripts/wheels/build-openssl.sh [PREFIX]     (default /opt/fastmm-openssl)
#
# OPENSSL_TARBALL=<path> uses a local copy of the tarball (still checked). JOBS sets make -j.
# A PREFIX that already holds this version and checksum is left as it is.
set -euo pipefail

OPENSSL_VERSION=3.5.8
OPENSSL_SHA256=a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2
PREFIX="${1:-/opt/fastmm-openssl}"
JOBS="${JOBS:-$(nproc)}"
stamp="$PREFIX/.fastmm-openssl-$OPENSSL_VERSION-$OPENSSL_SHA256"

if [[ -f "$stamp" ]]; then
  echo "build-openssl: OpenSSL $OPENSSL_VERSION already in $PREFIX"
  exit 0
fi

# OpenSSL's Configure needs these Perl modules; manylinux_2_28 has only the minimal interpreter.
if ! perl -MIPC::Cmd -MFindBin -MFile::Compare -MFile::Copy -MTime::Piece -e 1 2>/dev/null; then
  if command -v dnf >/dev/null; then
    dnf -y -q install perl
  else
    echo "build-openssl: install Perl with IPC::Cmd, FindBin, File::Compare, File::Copy, Time::Piece" >&2
    exit 1
  fi
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tarball="$work/openssl-$OPENSSL_VERSION.tar.gz"
if [[ -n "${OPENSSL_TARBALL:-}" ]]; then
  cp "$OPENSSL_TARBALL" "$tarball"
else
  curl -fsSL --retry 3 -o "$tarball" \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
fi
echo "$OPENSSL_SHA256  $tarball" | sha256sum -c -

tar -xzf "$tarball" -C "$work"
cd "$work/openssl-$OPENSSL_VERSION"
# no-module builds the providers into libcrypto. --openssldir under PREFIX: the wheel reads no
# openssl.cnf from the user's system unless OPENSSL_CONF names one; CA certificates are found by
# src/net/ca_locations.cpp.
./Configure linux-x86_64 --prefix="$PREFIX" --libdir=lib --openssldir="$PREFIX/ssl" \
  no-shared no-module no-tests no-docs no-apps -fPIC
make -j"$JOBS" build_libs >/dev/null
make install_dev >/dev/null
touch "$stamp"
echo "build-openssl: installed OpenSSL $OPENSSL_VERSION to $PREFIX"
