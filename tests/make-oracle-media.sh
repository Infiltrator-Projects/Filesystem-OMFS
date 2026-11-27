#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The original independent formatter is an oracle on disposable images,
# never a dependency of OMFS.so or BOOT. Pin its exact source revision.
set -euo pipefail
src=$(realpath "${1:?pinned omfsprogs checkout}")
out=$(realpath -m "${2:?output directory}")
test "$(git -C "$src" rev-parse HEAD)" = e77827eb5520b3e14fa8ed1eb6516f2d998c17e4
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -C "$src" archive HEAD | tar -xf - -C "$work"
# Fix only the old test oracle's uninitialized mutex and bitmap-buffer
# lengths. Keep its format structures, checksums and layout unchanged.
sed -i 's/omfs_info_t info;/omfs_info_t info = { .dev_mutex = PTHREAD_MUTEX_INITIALIZER };/' "$work/create_fs.c"
sed -i '1i#include <pthread.h>' "$work/create_fs.c"
sed -i 's/calloc(1, bitmap_size)/calloc(1, (size_t)(first_blk - BITMAP_BLK) * block_size)/' "$work/create_fs.c"
sed -i 's/i < bsize \* 8; i++, bitmap_blk++/i < bsize; i++, bitmap_blk++/;s/if (size != count)/if ((size_t)blocksize != count)/' "$work/libomfs/omfs.c"
make -C "$work" mkomfs
cc -std=gnu11 -D_FILE_OFFSET_BITS=64 -I"$work/libomfs" "$root/tests/oracle-populate.c" "$work/libomfs/libomfs.a" -pthread -o "$work/populate"
mkdir -p "$out"
test ! -e "$out/media.img"
truncate -s 32M "$out/media.img"
printf 'y\n' | "$work/mkomfs" -b 4096 -c 8 "$out/media.img"
dd if=/dev/urandom of="$out/proof.bin" bs=1 count=8229 status=none
"$work/populate" "$out/media.img" "$out/proof.bin"
