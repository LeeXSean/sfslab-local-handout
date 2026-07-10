/** This file defines types and constants related to the "on-disk"
    data structures that is the SFS file system.  That is, everything
    in this file is connected somehow to the data actually stored in
    an SFS disk image.

    The developer branch extends the original format with a reserved empty-file
    sentinel.  Keep sfs-fsck.c and sfs-disk.c synchronized with any further
    changes here. */

#ifndef SFS_DISK_H_
#define SFS_DISK_H_ 1

#include <stdint.h>

/** An SFS file system is divided into "blocks" each of which is
    this many bytes.  */
#define SFS_BLOCK_SIZE 512

/** You can identify an SFS disk image because its first 8 bytes are
    always this string (*including* the terminating NUL).  They are
    called a "magic number" for the format.

    In case you're wondering, "\xB2\xB1\xB3" is "213" with the high
    bit of each byte set.  We did this because, if your file format
    cannot be reasonably interpreted as text, it's important to put
    some bytes that have their high bits set, but which do _not_ add
    up to a valid UTF-8 character, in its magic number.

    The trailing \x01 is a version number for the format.  If the
    format ever has to change in a way that makes old programs unable
    to read it, this number will be incremented.  */
#define SFS_DISK_MAGIC "SFS\xB2\xB1\xB3\x01"

/** Each block of a SFS disk image, except the super block, has one of
    these codes as its first four bytes.  Unlike SFS_DISK_MAGIC,
    the codes do NOT include a terminating NUL.  */
#define SFS_BLOCK_TYPE_FREE "SFU\xF5" // block is unallocated
#define SFS_BLOCK_TYPE_FILE "SFF\xE6" // block holds (part of) a file
#define SFS_BLOCK_TYPE_DIR "SFD\xE4"  // block holds directory entries

/** Block IDs are 32-bit unsigned (little-endian) numbers.  Block N is at
    offset SFS_BLOCK_SIZE * N from the beginning of the filesystem.  Thus,
    the super block is block 0, but it is never referred to as such.  When
    a 'block_id' field of any on-disk structure has the value 0, this means
    the same thing as a NULL pointer in memory -- "end of list", "absent",
    "not in use", things like that.  */
typedef uint32_t block_id;

/** Developer-branch encoding for a live empty file with no data block.
    Valid block IDs are always smaller than the uint32_t block count, so this
    value can never name a mapped block. */
#define SFS_EMPTY_FILE_BLOCK UINT32_MAX

/** The superblock stores the number of blocks in a uint32_t, so the largest
    representable image contains UINT32_MAX blocks.  We presume that size_t
    and off_t can both hold this number.  */
#define SFS_MAX_DISK_SIZE ((size_t)UINT32_MAX * SFS_BLOCK_SIZE)

/** The maximum size of a single file in SFS is capped by the 32-bit
    'size' field in a sfs_dir_entry_t.  */
#define SFS_MAX_FILE_SIZE ((size_t)UINT32_MAX)

/** Every block of an SFS file system, *except* for the super block, starts
    with a 12-byte header laid out according to this struct. */
typedef struct sfs_block_hdr_t
{
    unsigned char type[4]; /**< one of the SFS_BLOCK_TYPE_* constants */
    block_id prev_block;   /**< previous block in the allocation chain or
                                free list containing this block */
    block_id next_block;   /**< next block in the allocation chain or
                                free list containing this block */
} sfs_block_hdr_t;

/** Amount of file data that can be stored in any one block.  */
#define BLOCK_DATA_SIZE ((uint32_t)(SFS_BLOCK_SIZE - sizeof(sfs_block_hdr_t)))

/** A block containing file data is laid out according to this struct.  */
typedef struct sfs_block_file_t
{
    sfs_block_hdr_t h;
    char data[BLOCK_DATA_SIZE];
} sfs_block_file_t;

/** Maximum number of characters in a file name, _including_ a terminating NUL.
    Caution: This constant also appears in sfs-api.h.  */
#define SFS_FILE_NAME_SIZE_LIMIT 24

/** A directory entry is exactly 32 bytes long and holds three pieces of
    information: */
typedef struct sfs_dir_entry_t
{
    block_id first_block; /**< First block; 0 = unused, EMPTY sentinel = no data */
    uint32_t size;        /**< Size of file in bytes */
    char name[SFS_FILE_NAME_SIZE_LIMIT]; /**< NUL-terminated name */
} sfs_dir_entry_t;

/** Number of directory entries that can be stored in one block.  */
#define DIR_ENTRIES_PER_BLOCK ((SFS_BLOCK_SIZE / sizeof(sfs_dir_entry_t)) - 1)

/** A block containing directory entries is laid out according to this struct.
 */
typedef struct sfs_block_dir_t
{
    sfs_block_hdr_t h;
    char unused[sizeof(sfs_dir_entry_t) - sizeof(sfs_block_hdr_t)];
    sfs_dir_entry_t files[DIR_ENTRIES_PER_BLOCK];
} sfs_block_dir_t;

/** The super block -- the first block of the file system, from which
    everything else can be found -- does not contain a normal block header.
    Its entire contents are laid out according to *this* struct, instead.  */
typedef struct sfs_filesystem_t
{
    char magic[8];         /**< SFS_DISK_MAGIC, including NUL */
    uint32_t n_blocks;     /**< Size of file system in SFS_BLOCK_SIZE units */
    block_id freelist;     /**< First block in the list of unallocated blocks */
    block_id next_rootdir; /**< Next block in the list of blocks holding
                                entries in the root directory */

    char unused[sizeof(sfs_dir_entry_t) -
                (8 + sizeof(uint32_t) + 2 * sizeof(block_id))];

    sfs_dir_entry_t files[DIR_ENTRIES_PER_BLOCK];
} sfs_filesystem_t;

sfs_block_hdr_t *accessBlock(block_id id);
sfs_block_hdr_t *accessFreeBlock(block_id id);
sfs_block_file_t *accessFileBlock(block_id id);
block_id idOfBlock(const sfs_block_hdr_t *blk);
sfs_filesystem_t *accessSuperBlock(void);
int getSFSStatus(void);
int sfs_has_open_files(void);
void setBlockType(sfs_block_hdr_t *blk, const char *type);

#endif
