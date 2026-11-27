# OMFS Common V2 migration

An independent read-only OMFS engine now exists. It supports bounded
bitmap-backed media, primary metadata CRC/XOR checks, nested directory
lookup and enumeration, non-contiguous file extents and continuation
tables, real bitmap-derived free capacity, label and read-only volume
lifetime. The same implementation builds both hosted and import-free
AMD64 firmware profiles; the hosted carrier embeds the actual firmware
ELF.

The native on-disk reference is the Linux v6.12.107 OMFS source retained
in [Filesystems](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/omfs).
No Linux VFS calls, external filesystem runtime, V1 API or BOOT-specific
format logic is used.

This is a development candidate. Byte-authored fixtures cover nested
names, fragmented reads, sector geometry, corrupt records and lifetime.
Hosted and firmware readers passed sanitizer and independent formatter
media tests (https://github.com/Infiltrator-Projects/Filesystem-OMFS/actions/runs/45947512074).
The oracle uses the pinned original formatter and library, with test-only
build and bitmap-buffer compatibility corrections. The BOOT signed OVMF
qualification has been added but is not yet verified. Mirror recovery,
bitmapless ReplayTV, global
allocation reconciliation, format, mutation, defragmentation and
release remain unqualified. These operations are not advertised.
