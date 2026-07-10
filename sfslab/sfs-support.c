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

static sfs_block_file_t *diskBlocks = NULL;
static size_t diskSizeInBytes = 0;

/** Given a pointer to a specific field of a structure, recover a
    pointer to the whole structure.  This can only be written as a
    macro, because the second and third arguments are not values.
    Code simplified from linux/kernel.h.  */
#define container_of(field_ptr, ct_type, field_name)                           \
    ((ct_type *)((char *)(field_ptr)) - offsetof(ct_type, field_name))

/** Set a block's type to TYPE, which should be one of the SFS_BLOCK_TYPE_*
    constants.  */
void setBlockType(sfs_block_hdr_t *blk, const char *type)
{
    memcpy(blk->type, type, sizeof blk->type);
}

/** Get a pointer to the header of the block with ID 'id'.  Note that
    there is no block with ID zero; in linked lists of blocks, zero
    should be treated as a null pointer.  (To get a pointer to the
    super block, use 'accessSuperBlock'.)  */
sfs_block_hdr_t *accessBlock(block_id id)
{
    assert(diskBlocks);
    assert(id < diskSizeInBytes / SFS_BLOCK_SIZE);
    if (id == 0)
        return NULL;
    return &diskBlocks[id].h;
}

/** Get a pointer to the block with ID 'id', verifying that it is
    a free block.  */
sfs_block_hdr_t *accessFreeBlock(block_id id)
{
    sfs_block_hdr_t *b = accessBlock(id);
    if (b != NULL)
        assert(memcmp(b->type, SFS_BLOCK_TYPE_FREE, sizeof b->type) == 0);
    return b;
}

/** Get a pointer to the block with ID 'id', verifying that it is
    a file-contents block.  */
sfs_block_file_t *accessFileBlock(block_id id)
{
    sfs_block_hdr_t *b = accessBlock(id);
    if (b != NULL)
    {
        assert(memcmp(b->type, SFS_BLOCK_TYPE_FILE, sizeof b->type) == 0);
        return container_of(b, sfs_block_file_t, h);
    }
    return NULL;
}

/** Get the block ID corresponding to any valid block pointer.  */
block_id idOfBlock(const sfs_block_hdr_t *blk)
{
    const char *p = (const char *)blk;
    const char *base = (const char *)diskBlocks;
    assert(base);
    assert(p >= base);
    size_t offset = (size_t)(p - base);
    assert(offset < diskSizeInBytes);
    assert(offset % SFS_BLOCK_SIZE == 0);
    return (block_id)(offset / SFS_BLOCK_SIZE);
}

/** Get a pointer to the super block, which is in fact the zeroth
    entry in the diskBlocks array. */
sfs_filesystem_t *accessSuperBlock(void)
{
    assert(diskBlocks != NULL);
    return (sfs_filesystem_t *)diskBlocks;
}

int getSFSStatus(void)
{
    if (diskBlocks == NULL)
       return -EINVAL;
    return 0;
}

int sfs_format(const char *diskName, size_t diskSize)
{
    if (diskBlocks != NULL)
        return -EBUSY;

    // Since we mmap the disk image, its size must be a multiple of
    // the system page size, even though the format only requires it to
    // be a multiple of SFS_BLOCK_SIZE.
    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
    assert(pagesize != (size_t)-1);
    assert(pagesize % SFS_BLOCK_SIZE == 0);

    if (diskSize == 0 || diskSize % pagesize != 0)
        return -EINVAL;
    if (diskSize > SFS_MAX_DISK_SIZE)
        return -EFBIG;

    int diskfd = open(diskName, O_RDWR | O_CREAT | O_TRUNC, DEFFILEMODE);
    if (diskfd < 0)
        return -errno;
    if (ftruncate(diskfd, (off_t)diskSize) < 0)
    {
        int err = -errno;
        close(diskfd);
        return err;
    }

    void *mapping =
        mmap(NULL, diskSize, PROT_READ | PROT_WRITE, MAP_SHARED, diskfd, 0);
    if (mapping == MAP_FAILED)
    {
        int err = -errno;
        close(diskfd);
        return err;
    }

    // The memory mapping we just created will still exist after this
    // file descriptor is closed.
    close(diskfd);

    diskBlocks = mapping;
    diskSizeInBytes = diskSize;

    // Because we opened the file with O_TRUNC and then enlarged it to the
    // desired size with ftruncate, we can be sure that every byte of the
    // file is currently '\0'.
    sfs_filesystem_t *superBlock = accessSuperBlock();
    memcpy(superBlock->magic, SFS_DISK_MAGIC, sizeof superBlock->magic);

    uint64_t n_blocks = diskSize / SFS_BLOCK_SIZE;
    assert(n_blocks <= (uint64_t)UINT32_MAX);

    superBlock->n_blocks = (uint32_t)n_blocks;
    superBlock->freelist = 1;
    for (block_id idx = 1; idx < n_blocks; idx++)
    {
        sfs_block_hdr_t *currBlock = accessBlock(idx);
        setBlockType(currBlock, SFS_BLOCK_TYPE_FREE);
        currBlock->prev_block = idx - 1;
        currBlock->next_block = (idx + 1 == n_blocks) ? 0 : idx + 1;
    }

    return 0;
}

int sfs_mount(const char *diskName)
{
    if (diskBlocks != NULL)
        return -EBUSY;

    int diskfd = open(diskName, O_RDWR);
    if (diskfd < 0)
        return -errno;

    struct stat diskst;
    if (fstat(diskfd, &diskst) < 0)
    {
        int err = -errno;
        close(diskfd);
        return err;
    }

    // The superblock's 32-bit block count bounds the image size.
    if (diskst.st_size > (off_t)SFS_MAX_DISK_SIZE)
    {
        close(diskfd);
        return -EFBIG;
    }

    // Since we mmap the disk image, its size must be a multiple of
    // the system page size, even though the format only requires it to
    // be a multiple of SFS_BLOCK_SIZE.
    size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
    assert(pagesize != (size_t)-1);
    assert(pagesize % SFS_BLOCK_SIZE == 0);

    if ((size_t)diskst.st_size % pagesize != 0)
    {
        close(diskfd);
        return -EINVAL;
    }

    sfs_filesystem_t super;
    ssize_t nread = pread(diskfd, &super, sizeof super, 0);
    if (nread < 0)
    {
        int err = -errno;
        close(diskfd);
        return err;
    }
    if (nread != (ssize_t)sizeof super ||
        memcmp(super.magic, SFS_DISK_MAGIC, sizeof super.magic) != 0 ||
        (size_t)super.n_blocks * SFS_BLOCK_SIZE != (size_t)diskst.st_size)
    {
        close(diskfd);
        return -EINVAL;
    }

    size_t imageSize = (size_t)diskst.st_size;
    void *mapping = mmap(NULL, imageSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                         diskfd, 0);
    if (mapping == MAP_FAILED)
    {
        int err = -errno;
        close(diskfd);
        return err;
    }
    close(diskfd);
    diskBlocks = mapping;
    diskSizeInBytes = imageSize;
    return 0;
}

int sfs_unmount(void)
{
    if (diskBlocks == NULL)
        return 0;

    if (sfs_has_open_files())
        return -EBUSY;

    // The munmap length must be the size we actually mapped.  The
    // superblock's n_blocks field is on-disk data that is not
    // guaranteed to match the mapping, and multiplying it by
    // SFS_BLOCK_SIZE happens in 32-bit arithmetic, which overflows
    // for images of 4 GiB and larger.
    //
    // munmap could conceivably report an I/O error.  If it does,
    // the file has been unmapped anyway (same principle as close().)
    int status = munmap(diskBlocks, diskSizeInBytes);
    diskBlocks = NULL;
    diskSizeInBytes = 0;
    
    return status ? -errno : 0;
}
