# OMFS: technical reference

## Historical context and format scope

SonicBlue's Optimized MPEG File System was used by ReplayTV recorders and the Rio Karma portable player. The format is organised around streaming media workloads, extent allocation and hash-table directories. It does not interpret MPEG compression or require files to be MPEG streams. Its device-specific design omits some general-purpose Unix metadata semantics; a host driver may synthesize ownership and permission values. The following description uses the independently reverse-engineered format documented by the Linux OMFS implementation and omfsprogs. The original vendor specification is not assumed to be available.

## Physical geometry

OMFS distinguishes ordinary allocation blocks from system blocks carrying metadata. They share the same block-address namespace, even if the metadata object uses fewer bytes than an allocated data block. A physical allocation address is multiplied by the normal block size, not the smaller system-record size. Unused space in a smaller system block is not a separate free allocation. The known device profiles commonly use 2048 to 8192-byte blocks; independently verify each supported block-size and mirror configuration. Bound the partition offset, address and length before reading. Metadata and allocation fields are stored big-endian.

The retained Linux header's implementation bounds include 8192-byte blocks and eight-block clusters; those limits are not necessarily universal original-format limits. Validate metadata offsets and continuation ownership before interpreting payload.

## Superblock field inventory

The superblock's identifying fields begin at byte 256. The 64-bit root-block address is at 256, total allocation-block count at 264, magic 0xC2993D87 at 272, block bytes at 276, metadata mirror count at 280 and system-block bytes at 284. Read at least 288 bytes before interpreting these fields. Require plausible block size, consistent system-block size, a bounded root address and a volume extent within the supplied image. The root metadata record independently repeats important geometry; disagreement must not be silently resolved by picking the more convenient value.

## System header and integrity

A system-block header is 24 bytes: self block address at 0 (64 bits), body size at 8 (32 bits), CRC-CCITT at 12 (16 bits), two reserved bytes at 14, version at 16, record type at 17, magic 0xD2 at 18, XOR check at 19 and reserved word at 20. The XOR is over the preceding 19 header bytes. Body CRC covers the declared body bytes after the header. Verify body size against the system-block boundary, identity and type before using pointers. The known version is 1. Consecutive mirrored metadata blocks must be checked independently; validate identity conventions and do not treat matching copies as proof that their contents are valid.

## Root block and bitmap

The root block starts with the common header. Its 64-bit total block count is at offset 32, root-directory address at 40, allocation-bitmap address at 48, block bytes at 56 and cluster-size setting at 60. Mirror count occupies eight bytes at 64 and the 256-byte volume label begins at 72. Validate root and bitmap addresses and both copies of geometry. Compute bitmap capacity from volume blocks, excluding padding bits and any reserved metadata copies. Preserve the device's bitmap bit numbering and allocation convention; verify against an independent formatted image before enabling writes.

## Inodes and object identity

A file or directory occupies a metadata object with the 24-byte common header. Its parent address is at 24, next sibling address at 32 and 64-bit timestamp in milliseconds at 40. Object type byte at 83 distinguishes D for directory and F for file. Reserved fields must be preserved. The 256-byte filename field begins at 152 and logical file size at 408. Normal, continuation and system headers use type bytes e, c and s respectively. The object's self address is its storage identity; there is no separate conventional Unix inode-number table. Names must fit their fixed storage without unbounded string scanning.

## Hashed directory algorithms

Directory bucket pointers start at byte 0x1B8 within the directory system block; each pointer is eight bytes. Derive bucket count from the validated available directory-record size. An all-ones pointer denotes an empty bucket. Compute the exact original name hash and bucket reduction; walk the sibling chain comparing names and checking each child's parent identity. An insertion links a new child at the bucket head, preserving the old head as its sibling. Deletion updates the bucket head or predecessor; moving a child must also update its parent. Bound chain length and reject cycles. Use the original hash implementation as a normative compatibility cross-check rather than substituting a modern hash.

## Extent tables and continuation

The first extent table starts at inode byte 0x1D0; continuation records place their table at 0x40. A table has a 64-bit next pointer at 0, 32-bit record count at 8, reserved word at 12 and entries starting at 16. Each entry is two 64-bit values: physical first block and run block count. The final entry is a terminator: all-ones first address and the ones-complement of the total number of data blocks described by that table. Validate the count against actual record capacity, recompute the sum and follow a continuation only after checking its system header. Reject repeated continuation pointers, out-of-range runs and overlapping physical ownership.

## Worked byte mapping

With 4096-byte blocks, consider an extent at block 100 of length 3 and another at block 900 of length 5. The first covers logical bytes 0 through 12287. Logical byte 15000 falls 2712 bytes into the second extent, so the physical byte offset is 900 times 4096 plus 2712, or 3689112. Logical offsets beyond the file size are not visible even if allocation extends further. A continuation header is metadata and is never delivered as file data. Missing or invalid mapping inside a declared file is corruption, not permission to expose unallocated media.

## Mutation, relocation and recovery

Allocation must reserve the whole required run and system-block mirror group before metadata publication. Data extension can grow the final run if adjacent blocks are free, or append another extent and possibly allocate a continuation table. Maintain the terminator and file length independently. A defragmenter copies and verifies source bytes into an available destination before publishing new mapping records. The format's CRC and mirrored metadata help detect damage but are not a general transactional journal. Validate interrupted-update ordering, mirror repair and allocation ownership against independent device images before writable qualification.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## Reference and test requirements

Independent references: Linux OMFS disk-format notes at https://docs.kernel.org/6.12/filesystems/omfs.html and implementation at https://github.com/torvalds/linux/tree/v6.12/fs/omfs . Original reverse-engineering utilities: https://bobcopeland.com/karma/ . Test differing metadata and allocation block sizes, all-ones sentinels, collision chains, continuation overflow, mirror disagreement, CRC failure, invalid total counts and partial last extents. Compare files against independent device images and omfsck. The exact directory hash, bitmap order, checksum initialisation and mirror write policy need fixture-level confirmation before independent writer compatibility is declared. Historical provider status is identified in Implementation and qualification boundary.

The historical source identifies magic 0xc2993d87 and inode marker 0xd2; directory/extent offsets 0x1b8/0x1d0 and continuation offset 0x40 must be interpreted under the admitted profile.

Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/omfs/linux/omfs_fs.h (blob 1ff6b9e412).
