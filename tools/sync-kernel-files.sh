#!/bin/sh
# Copy the open-gpu-kernel-modules files nvchan builds against into
# third_party/open-gpu-kernel-modules. Run after the kernel tree changes a
# file listed in FILES or moves to a new driver version, then commit.
#
#   tools/sync-kernel-files.sh [path/to/open-gpu-kernel-modules]
set -eu
top=$(cd "$(dirname "$0")/.." && pwd)
src=${1:-$top/../open-gpu-kernel-modules}
dst=$top/third_party/open-gpu-kernel-modules
[ -f "$src/src/common/unix/nvidia-push/src/nvidia-push.c" ] || {
    echo "not an open-gpu-kernel-modules tree: $src" >&2; exit 1; }
while read -r f; do
    mkdir -p "$dst/$(dirname "$f")"
    cp "$src/$f" "$dst/$f"
done < "$dst/FILES"
rev=$(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo unknown)
ver=$(sed -n 's/.*NV_VERSION_STRING *"\(.*\)"/\1/p' "$dst/src/common/inc/nvUnixVersion.h")
printf '%s %s\n' "$ver" "$rev" > "$dst/VERSION"
echo "synced $ver ($rev)"
