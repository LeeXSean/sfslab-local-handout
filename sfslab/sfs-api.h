/** This file defines the API exposed by sfs-disk.c.  Before you begin
    working on the assignment, you should read the doc comment above
    each function carefully.  You are implementing this specification;
    the doc comment for each function tells you what it is supposed to do.

    Students should not change this file.  The developer branch extends the
    course-shaped API with the explicitly marked, unscored size operations. */

#ifndef SFS_API_H_
#define SFS_API_H_ 1

#include <sys/types.h>

/** Maximum number of characters in a file name, _including_ a terminating NUL.
    Caution: This constant also appears in sfs-disk.h.  */
#define SFS_FILE_NAME_SIZE_LIMIT 24

/** An opaque object used by sfs_list to keep track of its position
    within the directory and handle concurrent modifications.  See
    sfs_list for how this is used.  */
typedef void *sfs_list_cookie;

/** Create and format a SFS disk image, then set it as the disk image
    being accessed by the other sfs-disk routines.  Allocate 'diskSize'
    bytes of space for the disk image (somewhat less than this will be
    usable for file data).  'diskSize' must be a multiple of the
    system page size, which you can find by calling sysconf(_SC_PAGESIZE)).

    The file 'diskName' will be created if it doesn't already exist.
    If it does exist, its old contents will be erased.  Caution: the
    old contents of an existing file might get erased even if this
    function fails.

    If a disk image is already active, this function will return
    -EBUSY and do nothing else.

    Returns 0 on success, or a negative error code.  */
int sfs_format(const char *diskName, size_t diskSize);

/** Load an existing SFS disk image and set it as the disk image being
    accessed by the other sfs-disk routines.  ("Mount" is the
    traditional name for this operation, see 'man 8 mount'.)  The
    contents of the image are _not_ erased, but they can be modified
    by subsequent calls to other sfs-disk routines.

    If a disk image is already active, this function will return
    -EBUSY and do nothing else.

    Return 0 on success, or a negative error code.  */
int sfs_mount(const char *diskName);

/** Deactivate the currently active disk image.  After you do this,
    you must call sfs_format or sfs_mount before you can use the rest
    of the sfs-disk routines again.

    This function will fail if any files are open within the disk
    image.  If there isn't an active disk image, it will do nothing
    and return 0.

    Returns 0 on success, or a negative error code.
    The error code -EBUSY means that files were open within the disk
    image, and therefore the image is still active.
    The error code -EIO indicates something went wrong while writing
    the disk image to persistent storage; in this case the image is
    _not_ active anymore.
    Other error codes come from the munmap() system call and probably
    indicate a bug in sfs-disk.c.  Contact course staff for advice if
    this happens.  */
int sfs_unmount(void);

/** Open a file within the currently active disk image.
    Unlike the 'open' system call, you don't have to say whether you
    want to read or write; you can do both.  If the file doesn't
    exist, it is created.

    An empty file name is invalid and returns -EINVAL.
    If creating a new file fails, no directory entry or data block is retained.
    Empty files consume no data block, and the root directory grows by chaining
    directory blocks when its embedded entries are full.

    Return a non-negative "file descriptor" on success, or a negative
    error code.  (The "file descriptor" can only be used with other
    sfs-disk routines, not with real system calls.)  */
int sfs_open(const char *fileName);

/** Close a "file descriptor" returned by sfs_open.
    This function cannot fail.  If you call it with an argument that
    isn't a valid "file descriptor", it just doesn't do anything.  */
void sfs_close(int fd);

/** Read up to LEN bytes of data from "file descriptor" FD into the
    buffer BUF.   Advance the file position for FD past those bytes.
    Just like the 'read' system call, you might not get as many bytes
    as you asked for, for several different reasons, the most common
    reason being that there were fewer than LEN bytes of data between
    the starting file position for FD and the end of the file.

    Return the number of bytes that were actually read, or a negative
    error code.

    If the starting file position for FD is at the end of the file,
    this is not considered an error, but nothing is written to BUF
    and the return value is zero.  */
ssize_t sfs_read(int fd, char *buf, size_t len);

/** Write up to LEN bytes of data from the buffer BUF into the file
    referred to by "file descriptor" FD.  Advance the file position
    for FD past those bytes.  Make the file larger if necessary.
    Just like the 'write' system call, not all of BUF might get
    written to the file, for several different reasons, the most
    common reason being that you hit some kind of size limit after
    writing _some_ but not _all_ of the bytes.

    If the requested ending position cannot fit in SFS's 32-bit on-disk file
    size, return -EFBIG without writing anything or advancing the position.

    Return the number of bytes that were actually written, or a
    negative error code.  */
ssize_t sfs_write(int fd, const char *buf, size_t len);

/** Return the current size of the file referred to by FD.  This is SFS's
    compact equivalent of fstat(): SFS has no metadata beyond the file size.

    If FD is not valid, return -EBADF. */
ssize_t sfs_fstat(int fd);

/** Change the size of the file referred to by FD to LENGTH bytes.  Growing a
    file zero-fills the new region.  Shrinking releases whole blocks beyond
    the new end and clamps every open descriptor for the same file to the new
    end when necessary.

    The operation is atomic on allocation failure: -ENOSPC or -EFBIG leaves
    the file contents, size, and descriptor positions unchanged. */
int sfs_ftruncate(int fd, size_t length);

/** Return the current file position of "file descriptor" FD.  If FD
    is not a valid "file descriptor", return -EBADF; this is the
    only reason this function might fail.  */
ssize_t sfs_getpos(int fd);

/** Shift the current file position of "file descriptor" FD by DELTA
    bytes, forward or backward from whatever it was before the call.
    If this would make the current file position negative, it should
    be set to zero.  If this would make the current file position
    greater than the size of the file, it should be set to be exactly
    at the end of the file.  (This is different from what the 'lseek'
    system call does, to make your life simpler.)

    Return the new file position.  If FD is not a valid "file
    descriptor", return -EBADF; this is the only reason this
    function might fail.  */
ssize_t sfs_seek(int fd, ssize_t delta);

/** Delete the file named NAME.
    If the file is open, its name disappears immediately but existing
    descriptors remain usable; its blocks are reclaimed after the last close.
    Opening NAME again creates a distinct new file.

    Returns 0 on success, or a negative error code, such as:

    -ENOENT          The file already doesn't exist.
    -EINVAL          NAME is empty.
    -ENAMETOOLONG    NAME is too long for SFS.  */
int sfs_remove(const char *name);

/** Rename the file currently named OLD_NAME to NEW_NAME.

    If OLD_NAME and NEW_NAME are equal and the file exists, succeed without
    changing anything.  If NEW_NAME already exists, atomically delete it and
    replace it with the file named OLD_NAME.  "Atomically" means that there
    shouldn't be any gap in which a concurrent thread can observe
    NEW_NAME not existing.

    Empty names are invalid and return -EINVAL.

    Returns 0 on success, or a negative error code. */
int sfs_rename(const char *old_name, const char *new_name);

/** List the contents of the file system, one name at a time.
    You have to call this function in a loop, like this:

        sfs_list_cookie cookie = NULL;
        char filename[SFS_FILE_NAME_SIZE_LIMIT];
        int status;
        while ((status = sfs_list(&cookie, filename,
                                  SFS_FILE_NAME_SIZE_LIMIT)) == 0) {
          // do something with 'filename' here
        }
        if (status < 0) {
          // error
        }

    You must set 'cookie' to NULL before the first call -- that's how
    sfs_list knows it's the first call.  sfs_list will store data in
    'cookie' to track its position within the directory.  When
    sfs_list eventually returns a nonzero 'status', it will also set
    'cookie' to NULL again.

    Once you start looping over file names, you must keep going until
    sfs_list returns a nonzero 'status'.  Failure to do this may cause
    deadlocks.  Deadlocks can also happen if you try to create,
    delete, or rename files from inside the loop.

    The return value is zero if and only if a file name was written to
    'filename_out'.  When the loop should stop -- when all the file
    names that there are have already been retrieved -- the return
    value is +1.  Finally, a negative error code is also possible,
    albeit only under unusual circumstances (e.g. calling sfs_list
    without having called sfs_mount first).  */
int sfs_list(sfs_list_cookie *cookie, char filename_out[],
             size_t filename_space);

#endif
