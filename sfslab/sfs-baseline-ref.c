//
// SFS Disk - Shark File System Implementation
//
// The Shark File System is a simple, FAT-style file system that is
//   designed to illustrate the basic principles of a file system for
//   students and be extensible.  The file system relies on the "disk"
//   being memory mapped so updates to memory can be sent to durable
//   storage by the operating system.  The file system only supports
//   the bare minimum set of features - files can be accessed by name,
//   created, deleted, renamed, read from, written to.  A "production"
//   filesystem includes many other features, such as permissions,
//   access time, nested directories, long file names, etc.
//
// The disk is "formatted" into 512-byte blocks.  Each block is linked
//   to the other blocks, similar to a free list in malloc.
//
// The file system relies on the first block being the "superblock".
//   This special block contains the information about the disk being
//   used and can locate the root directory.  Under the current
//   implementation, there is only one directory, embedded in the
//   superblock; however, this could be changed by adding support for
//   a special file type of "directory" and modifying the file lookup
//   algorithm for opening a file.
//
// A file consists of one or more "disk blocks" which are chunks of 512
//   bytes.  Each block links to the one before and after it in the file,
//   which provide 500 bytes of space per allocated block.  The end of
//   the file is known by both the size of the file and the last block
//   links to block 0, which is "NULL".  A modern file system might
//   instead use a B-tree or other structure to manage the allocated
//   blocks.
//
// Open files are tracked using a two-level structure.  One level is the
//   open file descriptor tracking the position in the file for that
//   descriptor.  It links to a separate table that has a single entry
//   per file, which provides the current size of the file and the
//   reference count, so that a file could not be deleted while it is
//   still open.
//
// @author Brian Railing (bpr@cs.cmu.edu)
//

#include "sfs-disk.h"
#include "sfs-api.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>

static pthread_mutex_t sfs_baseline_lock = PTHREAD_MUTEX_INITIALIZER;

#define BASELINE_LOCK()   pthread_mutex_lock(&sfs_baseline_lock)
#define BASELINE_UNLOCK() pthread_mutex_unlock(&sfs_baseline_lock)

/** Maximum number of files that can exist.  Since we have not implemented
    expansion of the root directory, this is equal to the number of directory
    entries that can fit in a single disk block.  */
#define FILE_COUNT_LIMIT DIR_ENTRIES_PER_BLOCK

/** Maximum number of open files.  This is intentionally larger
    than the number of files that can exist; some of the traces
    open the same file more than once.  */
#define OPEN_FILE_LIMIT 32

static_assert(sizeof(sfs_block_file_t) == SFS_BLOCK_SIZE,
              "SFS_BLOCK_SIZE and sfs_block_file_t are out of sync");
static_assert(sizeof(sfs_block_dir_t) == SFS_BLOCK_SIZE,
              "SFS_BLOCK_SIZE and sfs_block_dir_t are out of sync");
static_assert(sizeof(sfs_filesystem_t) == SFS_BLOCK_SIZE,
              "SFS_BLOCK_SIZE and sfs_filesystem_t are out of sync");

/** This struct corresponds to what CS:APP calls a "v-node table" entry. */
typedef struct sfs_mem_file_t
{
    uint32_t refCount;
    int fileEntryIdx;
    sfs_dir_entry_t *diskFile;
} sfs_mem_file_t;

/** This struct corresponds to what CS:APP calls an "open file table" entry.
    The "descriptor table" is the openFileDescTable array itself. */
typedef struct sfs_mem_filedesc_t
{
    sfs_mem_file_t *fileEntry;
    block_id startBlock;
    block_id currBlock;
    size_t currPos;
} sfs_mem_filedesc_t;

static_assert(sizeof SFS_DISK_MAGIC == offsetof(sfs_filesystem_t, n_blocks),
              "'type' field of sfs_filesystem_t does not match SFS_DISK_MAGIC");

static sfs_mem_file_t *openFileTable[FILE_COUNT_LIMIT];
static sfs_mem_filedesc_t *openFileDescTable[OPEN_FILE_LIMIT];

//
// Internal subroutines
//

/** Round up the size_t value SIZE to the nearest multiple of N.
    Special case: for all nonzero k, roundUp(k*N, N) returns k*N,
    but roundUp(0, N) returns N.  */
static size_t roundUp(size_t size, size_t n)
{
    if (size == 0)
        size = 1;
    return n * ((size + (n - 1)) / n);
}

/** Return the smaller of two size_t quantities. */
static size_t sizeMin(size_t a, size_t b)
{
    return a < b ? a : b;
}

/** Allocate N_BLOCKS free blocks from the free list.  Set each newly
    allocated block's type to TYPE, and chain them all together.
    Return the block ID of the first block in the chain.

    If N_BLOCKS blocks are not currently available for allocation,
    leaves the free list unchanged and returns 0.  Also returns 0
    if N_BLOCKS is zero.  */
static block_id allocateBlocks(uint32_t n_blocks, const char *type)
{
    sfs_filesystem_t *super = accessSuperBlock();
    if (super->freelist == 0 || n_blocks == 0)
        return 0;

    block_id first_alloc_id = super->freelist;
    sfs_block_hdr_t *first_alloc_blk = accessFreeBlock(first_alloc_id);

    block_id last_alloc_id = first_alloc_id;
    sfs_block_hdr_t *last_alloc_blk = first_alloc_blk;

    for (uint32_t i = 1; i < n_blocks; i++)
    {
        if (last_alloc_blk->next_block == 0)
            return 0; // not enough free blocks available
        last_alloc_id = last_alloc_blk->next_block;
        last_alloc_blk = accessFreeBlock(last_alloc_id);
    }

    // At this point we know that we have n_blocks free blocks available.
    // 'first_alloc_blk' points to the first block being allocated and
    // 'last_alloc_blk' points to the last block being allocated (*not*
    // one beyond).
    block_id next_free_id = last_alloc_blk->next_block;
    if (next_free_id != 0)
    {
        sfs_block_hdr_t *next_free_blk = accessFreeBlock(next_free_id);
        next_free_blk->prev_block = 0;
        last_alloc_blk->next_block = 0;
    }
    super->freelist = next_free_id;

    for (sfs_block_hdr_t *b = first_alloc_blk; b;
         b = accessFreeBlock(b->next_block))
        setBlockType(b, type);

    return first_alloc_id;
}

/** Deallocate all of the blocks in the allocation chain starting at
    'first_block'; that is, move them to the free list and change their type
    to SFS_BLOCK_TYPE_FREE.  'first_block' does not have to be the very
    first block in an allocation chain.  */
static void freeBlocks(block_id first_block)
{
    sfs_filesystem_t *superBlock = accessSuperBlock();
    sfs_block_hdr_t *b = accessBlock(first_block);
    if (b->prev_block != 0)
    {
        sfs_block_hdr_t *p = accessBlock(b->prev_block);
        p->next_block = 0;
        b->prev_block = 0;
    }

    for (;;)
    {
        assert(memcmp(b->type, SFS_BLOCK_TYPE_FREE, sizeof b->type) != 0);
        setBlockType(b, SFS_BLOCK_TYPE_FREE);
        if (b->next_block == 0)
            break;
        b = accessBlock(b->next_block);
    }
    b->next_block = superBlock->freelist;
    if (superBlock->freelist != 0)
    {
        // The free list is doubly linked (sfs-fsck checks this), so the
        // old head's prev pointer must be re-aimed at the chain we are
        // prepending; otherwise the list is left inconsistent.
        sfs_block_hdr_t *oldHead = accessFreeBlock(superBlock->freelist);
        oldHead->prev_block = idOfBlock(b);
    }
    superBlock->freelist = first_block;
}

/** Allocate an open-file-table entry and "file descriptor" referring
    to an existing file on disk whose directory entry is at index
    'entryIndex'.  */
static int addOpenFileEntry(int entryIndex)
{
    sfs_mem_filedesc_t *memDescFile = NULL;
    int fd = -1;
    for (int idx = 0; idx < OPEN_FILE_LIMIT; idx++)
    {
        if (openFileDescTable[idx] == NULL)
        {
            memDescFile = malloc(sizeof(sfs_mem_filedesc_t));
            if (memDescFile == NULL)
            {
                return -ENOMEM;
            }
            fd = idx;
            break;
        }
    }
    if (memDescFile == NULL)
    {
        // No fd slots left.
        return -EMFILE;
    }

    sfs_mem_file_t *fileEntry = openFileTable[entryIndex];
    if (fileEntry == NULL)
    {
        fileEntry = malloc(sizeof(sfs_mem_file_t));
        if (fileEntry == NULL)
        {
            free(memDescFile);
            return -ENOMEM;
        }

        sfs_filesystem_t *superBlock = accessSuperBlock();
        fileEntry->diskFile = &superBlock->files[entryIndex];
        fileEntry->fileEntryIdx = entryIndex;
        fileEntry->refCount = 0;
        openFileTable[entryIndex] = fileEntry;
    }

    fileEntry->refCount += 1;
    memDescFile->fileEntry = fileEntry;
    memDescFile->startBlock = fileEntry->diskFile->first_block;
    memDescFile->currBlock = memDescFile->startBlock;
    memDescFile->currPos = 0;
    openFileDescTable[fd] = memDescFile;
    return fd;
}

int sfs_has_open_files(void)
{
    BASELINE_LOCK();

    int rv = 0;
    for (int idx = 0; idx < OPEN_FILE_LIMIT; idx++)
    {
        if (openFileDescTable[idx] != NULL) {
            rv = 1;
            goto out;
        }
    }

    // With no live descriptor entries, no per-file entries should remain.
    for (int idx = 0; (unsigned long)idx < FILE_COUNT_LIMIT; idx++)
    {
        assert(openFileTable[idx] == NULL);
    }

out:
    BASELINE_UNLOCK();
    return rv;
}

/** Create a new file named 'fileName' and return an open-file-table
    entry for it.  'emptyIndex' is known to be a free slot in the
    directory on disk.  */
static int createFile(const char *fileName, int emptyIndex)
{
    // Every file must occupy at least one block on disk, even if
    // it is empty.  This is because we use nonzero 'first_block'
    // to identify files that do exist.  (Optional puzzle for you:
    // find a way to avoid this, so that empty files consume only
    // a directory entry.)

    block_id startBlock = allocateBlocks(1, SFS_BLOCK_TYPE_FILE);
    if (startBlock == 0)
        return -ENOSPC;

    sfs_filesystem_t *superBlock = accessSuperBlock();
    sfs_dir_entry_t *sfe = &superBlock->files[emptyIndex];
    sfe->first_block = startBlock;
    sfe->size = 0;

    // Overlength names _should_ have been excluded at a higher level.
    size_t len = strlen(fileName);
    assert(len + 1 <= SFS_FILE_NAME_SIZE_LIMIT);

    // Copy the name and then clear the rest of the space.
    memcpy(sfe->name, fileName, len);
    memset(sfe->name + len, '\0', SFS_FILE_NAME_SIZE_LIMIT - len);

    return addOpenFileEntry(emptyIndex);
}

//
// SFS API functions begin here
// see sfs-api.h for documentation comments for these functions
//

int sfs_open(const char *fileName)
{
    BASELINE_LOCK();
    int rv;

    // Can only have 23 characters, because the string on disk is NUL
    // terminated.
    if (strnlen(fileName, SFS_FILE_NAME_SIZE_LIMIT + 1) + 1 >
        SFS_FILE_NAME_SIZE_LIMIT) {
        rv = -ENAMETOOLONG;
        goto out;
    }

    // Is a disk image available?
    if (getSFSStatus() < 0) {
        rv = -ENOMEDIUM;
        goto out;
    }

    sfs_filesystem_t *superBlock = accessSuperBlock();
    int fileEntry;
    int emptyEntry = -1;
    for (fileEntry = 0; (unsigned long)fileEntry < FILE_COUNT_LIMIT;
         fileEntry++)
    {
        if (superBlock->files[fileEntry].first_block != 0 &&
            strcmp(superBlock->files[fileEntry].name, fileName) == 0)
        {
            rv = addOpenFileEntry(fileEntry);
            goto out;
        }
        else if (emptyEntry == -1 &&
                 superBlock->files[fileEntry].first_block == 0)
        {
            emptyEntry = fileEntry;
        }
    }

    // Optional challenge: Allow the super block to be just the first
    // in a chain of directory blocks, and thus permit more than
    // FILE_COUNT_LIMIT files to exist.
    if (emptyEntry == -1) {
        rv = -ENOSPC;
        goto out;
    }

    rv = createFile(fileName, emptyEntry);
out:
    BASELINE_UNLOCK();
    return rv;
}

void sfs_close(int fd)
{
    BASELINE_LOCK();
    if (fd < 0 || fd >= OPEN_FILE_LIMIT) {
        BASELINE_UNLOCK();
        return;
    }
    sfs_mem_filedesc_t *tFile = openFileDescTable[fd];
    if (!tFile) {
        BASELINE_UNLOCK();
        return;
    }
    sfs_mem_file_t *fileEntry = tFile->fileEntry;
    openFileDescTable[fd] = NULL;
    free(tFile);

    fileEntry->refCount--;
    if (fileEntry->refCount > 0) {
        BASELINE_UNLOCK();
        return;
    }

    int idx = fileEntry->fileEntryIdx;
    free(fileEntry);
    openFileTable[idx] = NULL;
    BASELINE_UNLOCK();
}

ssize_t sfs_read(int fd, char *buf, size_t len)
{
    BASELINE_LOCK();
    ssize_t rv;

    if (fd < 0 || fd >= OPEN_FILE_LIMIT) {
        rv = -EBADF;
        goto out;
    }

    sfs_mem_filedesc_t *tFile = openFileDescTable[fd];
    if (tFile == NULL) {
        rv = -EBADF;
        goto out;
    }

    // We are going to read 'len' bytes, or the amount of data remaining
    // in the file, whichever is smaller.
    // This subtraction cannot produce a value larger than SSIZE_MAX
    // because it's impossible for a file in SFS to be that large.
    size_t fileSize = tFile->fileEntry->diskFile->size;
    size_t currPos = tFile->currPos;

    assert(currPos <= fileSize);
    size_t totalToRead = sizeMin(fileSize - currPos, len);

    size_t toRead = totalToRead;

    // Copy chunks of data from the mapped disk image to the caller's buffer.
    //
    // Each chunk is the smaller of:
    //  - the amount of data still to be read
    //  - the amount of data between currPos and the end of the current block
    // This number can be different from BLOCK_DATA_SIZE only for the
    // very first and the very last chunk of a read operation.
    //
    // Each chunk starts at the beginning of a disk block's data area,
    // except the very first chunk, which will begin in the middle of a
    // data area if the previous read or seek operation left the file
    // position not a multiple of BLOCK_DATA_SIZE.
    sfs_block_file_t *diskBlock = accessFileBlock(tFile->currBlock);
    size_t blockPos = currPos % BLOCK_DATA_SIZE;
    size_t chunkSize =
        sizeMin(roundUp(currPos, BLOCK_DATA_SIZE) - currPos, toRead);
    for (;;)
    {
        // The chunk size can be zero on the first iteration, if the
        // starting position was exactly at a block boundary.
        if (chunkSize > 0)
        {
            memcpy(buf, &diskBlock->data[blockPos], chunkSize);
            buf += chunkSize;
            toRead -= chunkSize;
        }
        if (toRead == 0)
            break;

        blockPos = 0;
        chunkSize = sizeMin(BLOCK_DATA_SIZE, toRead);
        diskBlock = accessFileBlock(diskBlock->h.next_block);
        // This could only happen legitimately if we were reading to the end
        // of a file whose size was an exact multiple of BLOCK_DATA_SIZE, but
        // then we would already have exited the loop.
        assert(diskBlock != NULL);
    }

    tFile->currBlock = idOfBlock(&diskBlock->h);
    tFile->currPos = currPos + totalToRead;

    rv = (ssize_t)totalToRead;
out:
    BASELINE_UNLOCK();
    return rv;
}

ssize_t sfs_write(int fd, const char *buf, size_t len)
{
    BASELINE_LOCK();
    ssize_t rv;

    if (fd < 0 || fd >= OPEN_FILE_LIMIT) {
        rv = -EBADF;
        goto out;
    }

    sfs_mem_filedesc_t *tFile = openFileDescTable[fd];
    if (tFile == NULL) {
        rv = -EBADF;
        goto out;
    }

    size_t fileSize = tFile->fileEntry->diskFile->size;
    size_t currPos = tFile->currPos;
    assert(currPos <= fileSize);

    // This implementation does not do a partial write if there is
    // insufficient space on disk for the complete write; it always
    // either writes all 'len' bytes, or none.
    size_t fileAllocSize = roundUp(fileSize, BLOCK_DATA_SIZE);
    size_t endPos = len + currPos;
    size_t toWrite = len;

    // If we need to enlarge the file, do so now, and if we can't make
    // it big enough, fail the whole operation.  Note that empty files
    // still have one allocated block: files of length [0, 500]
    // require one block, [501, 1000] require two, etc.  (Optional
    // challenge: Think of a way to make empty files not require any
    // allocated blocks.)
    block_id firstNewId = 0;
    if (endPos > fileAllocSize)
    {
        size_t fileNewAllocSize = roundUp(endPos, BLOCK_DATA_SIZE);
        if (fileNewAllocSize > SFS_MAX_FILE_SIZE) {
            rv = -EFBIG;
            goto out;
        }

        uint32_t addlBlocks =
            (uint32_t)((fileNewAllocSize - fileAllocSize) / BLOCK_DATA_SIZE);
        assert(addlBlocks >= 1);

        firstNewId = allocateBlocks(addlBlocks, SFS_BLOCK_TYPE_FILE);
        if (firstNewId == 0) {
            rv = -ENOSPC;
            goto out;
        }
    }

    // Copy chunks of data from the caller's buffer to the mapped disk image.
    // See comments above the very similar loop in sfs_read() for more detail.
    sfs_block_file_t *diskBlock = accessFileBlock(tFile->currBlock);
    size_t blockPos = currPos % BLOCK_DATA_SIZE;
    size_t chunkSize =
        sizeMin(roundUp(currPos, BLOCK_DATA_SIZE) - currPos, toWrite);
    for (;;)
    {
        // The chunk size can be zero on the first iteration, if the
        // starting position was exactly at a block boundary.
        if (chunkSize > 0)
        {
            memcpy(&diskBlock->data[blockPos], buf, chunkSize);
            buf += chunkSize;
            toWrite -= chunkSize;
        }
        if (toWrite == 0)
            break;

        blockPos = 0;
        chunkSize = sizeMin(BLOCK_DATA_SIZE, toWrite);
        sfs_block_file_t *nextBlock = accessFileBlock(diskBlock->h.next_block);
        if (nextBlock == NULL)
        {
            // We should only get here once, at most, per write call.
            assert(firstNewId != 0);
            // We have just advanced the file position to the end of the
            // original allocation for the file.  Attach the additional
            // blocks beginning at 'firstNewId' to the end of the file,
            // and continue.
            nextBlock = accessFileBlock(firstNewId);
            diskBlock->h.next_block = firstNewId;
            nextBlock->h.prev_block = idOfBlock(&diskBlock->h);
            firstNewId = 0;
        }
        diskBlock = nextBlock;
    }

    tFile->currBlock = idOfBlock(&diskBlock->h);
    tFile->currPos = endPos;
    if (endPos > fileSize)
    {
        assert(endPos < SFS_MAX_FILE_SIZE);
        tFile->fileEntry->diskFile->size = (uint32_t)endPos;
    }
    rv = (ssize_t)len;
out:
    BASELINE_UNLOCK();
    return rv;
}

ssize_t sfs_getpos(int fd)
{
    BASELINE_LOCK();
    // It's your job as the student to implement this function.
    // See sfs-disk.h for the specification.
    BASELINE_UNLOCK();
    return -ENOSYS;
}

ssize_t sfs_seek(int fd, ssize_t delta)
{
    BASELINE_LOCK();
    // It's your job as the student to implement this function.
    // See sfs-disk.h for the specification.
    BASELINE_UNLOCK();
    return -ENOSYS;
}

int sfs_remove(const char *name)
{
    BASELINE_LOCK();
    int rv;

    // Can only have 23 characters, because the string on disk is NUL
    // terminated.
    if (strnlen(name, SFS_FILE_NAME_SIZE_LIMIT + 1) + 1 >
        SFS_FILE_NAME_SIZE_LIMIT) {
        rv = -ENAMETOOLONG;
        goto out;
    }

    // Is a disk image available?
    if (getSFSStatus() < 0) {
        rv = -ENOMEDIUM;
        goto out;
    }

    sfs_filesystem_t *superBlock = accessSuperBlock();
    for (int fileEntry = 0; (unsigned long)fileEntry < FILE_COUNT_LIMIT;
         fileEntry++)
    {
        if (superBlock->files[fileEntry].first_block != 0 &&
            strcmp(superBlock->files[fileEntry].name, name) == 0)
        {
            // Is this file currently open?
            if (openFileTable[fileEntry] != NULL)
            {
                // The Unix convention is, when you delete a file that's
                // open, it disappears from its directory, but its contents
                // survive until everyone has closed it.  SFS is not set up
                // to handle this, so instead we refuse to delete files
                // that are open.  Optional challenge for you: Change SFS so
                // it *can* do what Unix does.
                rv = -EBUSY;
                goto out;
            }
            block_id firstBlock = superBlock->files[fileEntry].first_block;
            superBlock->files[fileEntry].first_block = 0;
            freeBlocks(firstBlock);
            rv = 0;
            goto out;
        }
    }

    // If we get here, the file we were asked to delete does not exist.
    // The Unix convention is to report this as an error.  It would be
    // equally valid to report success -- we were asked to make the
    // file not exist, and indeed it doesn't!
    rv = -ENOENT;
out:
    BASELINE_UNLOCK();
    return rv;
}

int sfs_rename(const char *old_name, const char *new_name)
{
    BASELINE_LOCK();
    // It's your job as the student to implement this function.
    // See sfs-disk.h for the specification.
    BASELINE_UNLOCK();
    return -ENOSYS;
}

int sfs_list(sfs_list_cookie *cookie, char filename_out[],
             size_t filename_space)
{
    BASELINE_LOCK();
    int rv;

    // The API promises that whenever this function returns a nonzero
    // status -- errors included -- the cookie is reset to NULL, so a
    // caller can always start a fresh loop afterward.

    // Corner case: If filename_space is zero, we cannot produce
    // an empty string into it on error.
    if (filename_space == 0) {
        *cookie = NULL;
        rv = -EINVAL;
        goto out;
    }

    if (getSFSStatus() < 0) {
        *cookie = NULL;
        rv = -ENOMEDIUM;
        goto out;
    }

    // The cookie value is just an offset within the localFiles array,
    // cast to void*.  It could be necessary to change this in order
    // to make the filesystem thread-safe, and/or to increase the
    // number of files that can exist.  If you decide to change it,
    // keep in mind that the "cookie" argument is a pointer _to_ the
    // cookie value, not the cookie value itself.
    uintptr_t next_file_slot = (uintptr_t)*cookie;
    struct sfs_filesystem_t *superBlock = accessSuperBlock();
    while (next_file_slot < FILE_COUNT_LIMIT)
    {
        sfs_dir_entry_t *e = &superBlock->files[next_file_slot];
        if (e->first_block)
        {
            // Found a "live" directory entry.
            size_t len = strlen(e->name);
            if (len + 1 > filename_space)
            {
                *cookie = NULL;
                rv = -ENAMETOOLONG;
                goto out;
            }
            memcpy(filename_out, e->name, len + 1);
            *cookie = (void *)(next_file_slot + 1);
            rv = 0;
            goto out;
        }
        next_file_slot++;
    }

    // No more files to report.
    *cookie = NULL;
    rv = 1;
out:
    BASELINE_UNLOCK();
    return rv;
}
