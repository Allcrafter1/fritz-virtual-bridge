#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/install-freetz-package.sh /path/to/freetz-ng

Installs the FRITZ! Virtual Bridge package source into a compatible Freetz-NG
checkout. Existing unrelated Freetz files and configuration are not changed.
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$script_dir/.." && pwd)"
freetz_dir="$(cd -- "$1" && pwd)"
package_source="$project_dir/freetz/package"
package_target="$freetz_dir/make/pkgs/fritzvirtual"
archive_name="fritzvirtual-0.1.0.tar.gz"
archive_path="$freetz_dir/dl/$archive_name"

[[ -f "$freetz_dir/Makefile" && -d "$freetz_dir/make/pkgs" ]] || {
    echo "Not a Freetz-NG checkout: $freetz_dir" >&2
    exit 1
}

mkdir -p "$package_target" "$freetz_dir/dl"
cp "$package_source/Config.in" "$package_target/Config.in"
mkdir -p "$package_target/files"
cp -a "$package_source/files/." "$package_target/files/"

archive_tmp="$(mktemp -d)"
trap 'rm -rf "$archive_tmp"' EXIT
mkdir -p "$archive_tmp/fritzvirtual-0.1.0"
cp "$package_source/src/"*.c "$package_source/src/"*.h \
    "$archive_tmp/fritzvirtual-0.1.0/"
tar --sort=name --mtime='@1790985600' --owner=0 --group=0 --numeric-owner \
    -C "$archive_tmp" -czf "$archive_path" fritzvirtual-0.1.0

source_hash="$(sha256sum "$archive_path" | cut -d' ' -f1)"
sed "s/@SOURCE_HASH@/$source_hash/" \
    "$package_source/fritzvirtual.mk.in" >"$package_target/fritzvirtual.mk"

echo "Installed make/pkgs/fritzvirtual"
echo "Created dl/$archive_name ($source_hash)"
