/*
 * test-sfs.c - SFS Lab test driver and autograder
 *
 * Grading structure:
 *   Category A  (5 pts)  - Feature tests
 *   Category B  (4 pts)  - Sequential correctness
 *   Category C  (3 pts)  - Concurrent correctness (plus a TSan race sweep)
 *   Performance (10 pts) - Concurrent throughput vs. coarse-lock baseline
 *   Style       (4 pts)  - Manual review (not auto-graded)
 *
 * Build:  make
 * Run:    ./test-sfs     (from the handout directory)
 */

#include "sfs-api.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Result of the ThreadSanitizer sweep (see run_tsan_check). */
enum tsan_result
{
    TSAN_SKIPPED,      /* correctness gate not passed; TSan not attempted */
    TSAN_CLEAN,        /* ran, no races */
    TSAN_RACE,         /* data race detected */
    TSAN_TRACE_FAILED, /* sanitized C traces failed functionally */
    TSAN_UNAVAILABLE,  /* toolchain/environment cannot run TSan */
    TSAN_TIMEOUT,      /* sanitized run exceeded its budget */
};

/* Disk image paths. Default to the current directory; override with
   SFS_DISK_DIR=/some/path (e.g. SFS_DISK_DIR=/tmp on Docker Desktop
   to keep perf I/O off the bind mount -- see the writeup's notes on
   running inside Docker or WSL). */
static const char *DISK_NAME = "test.img";
static const char *CONC_DISK_C00 = "test_conc_C00.img";
static const char *CONC_DISK_C01 = "test_conc_C01.img";
static const char *CONC_DISK_C02_RW = "test_conc_C02_rw.img";
static const char *CONC_DISK_C02_STORM = "test_conc_C02_storm.img";
static const char *CONC_DISK_C02_DIR = "test_conc_C02_dir.img";
static const char *PERF_DISK = "test_perf.img";

static char *sfs_join(const char *dir, const char *name)
{
    size_t len = strlen(dir) + 1 + strlen(name) + 1;
    char *out = malloc(len);
    if (!out)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    snprintf(out, len, "%s/%s", dir, name);
    return out;
}

static void init_disk_paths(void)
{
    const char *dir = getenv("SFS_DISK_DIR");
    if (!dir || !*dir)
        return;
    DISK_NAME = sfs_join(dir, "test.img");
    CONC_DISK_C00 = sfs_join(dir, "test_conc_C00.img");
    CONC_DISK_C01 = sfs_join(dir, "test_conc_C01.img");
    CONC_DISK_C02_RW = sfs_join(dir, "test_conc_C02_rw.img");
    CONC_DISK_C02_STORM = sfs_join(dir, "test_conc_C02_storm.img");
    CONC_DISK_C02_DIR = sfs_join(dir, "test_conc_C02_dir.img");
    PERF_DISK = sfs_join(dir, "test_perf.img");
    fprintf(stderr, "Disk images redirected via SFS_DISK_DIR: %s\n", dir);
}

static void cleanup_concurrency_disks(void)
{
    unlink(CONC_DISK_C00);
    unlink(CONC_DISK_C01);
    unlink(CONC_DISK_C02_RW);
    unlink(CONC_DISK_C02_STORM);
    unlink(CONC_DISK_C02_DIR);
}

/* Keep a copy of a failing trace's disk image(s) for post-mortem fsck.
   On by default for human runs; off under --tsan-only so the sanitizer
   sweep stays clean.  SFS_KEEP_FAILED_DISKS=0/1 overrides.
   The copies match the *.img cleanup glob (make clean). */
static int keep_failed_disks;
static int quiet_mode;   /* -q: suppress all FAIL detail output */

static int copy_file_contents(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
    {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
    {
        ssize_t off = 0;
        while (off < n)
        {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0)
            {
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }
    if (n < 0)
        rc = -1;
    close(in);
    if (close(out) != 0)
        rc = -1;
    return rc;
}

/* Copy the failing trace's surviving disk image(s) to fail_<id>_<name>
   next to the originals.  Returns a malloc'd hint string (one CHECK-style
   line per kept image) for the caller to route into the trace's FAIL
   detail, or NULL if nothing was kept. */
static char *keep_failure_images(const char *id)
{
    if (!keep_failed_disks)
        return NULL;

    /* A/B traces share DISK_NAME; C traces use their own images.
       Restrict candidates so a leftover test.img from an earlier
       category is not misattributed to a concurrent trace. */
    const char *cands[5];
    size_t n_cands = 0;
    if (id[0] == 'C')
    {
        cands[n_cands++] = CONC_DISK_C00;
        cands[n_cands++] = CONC_DISK_C01;
        cands[n_cands++] = CONC_DISK_C02_RW;
        cands[n_cands++] = CONC_DISK_C02_STORM;
        cands[n_cands++] = CONC_DISK_C02_DIR;
    }
    else
    {
        cands[n_cands++] = DISK_NAME;
    }

    char *out = NULL;
    size_t outlen = 0;
    FILE *ms = open_memstream(&out, &outlen);
    if (!ms)
        return NULL;

    for (size_t k = 0; k < n_cands; k++)
    {
        if (access(cands[k], F_OK) != 0)
            continue;
        const char *base = strrchr(cands[k], '/');
        base = base ? base + 1 : cands[k];
        size_t dirlen = (size_t)(base - cands[k]);
        char dst[PATH_MAX];
        int m = snprintf(dst, sizeof dst, "%.*sfail_%s_%s",
                         (int)dirlen, cands[k], id, base);
        if (m <= 0 || (size_t)m >= sizeof dst)
            continue;
        if (copy_file_contents(cands[k], dst) == 0)
            fprintf(ms, "       -> kept failing disk image: %s "
                        "(inspect: ./sfs-fsck --dump %s)\n", dst, dst);
    }
    fclose(ms);
    if (outlen == 0)
    {
        free(out);
        return NULL;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/*  Per-trace check infrastructure                                     */
/* ------------------------------------------------------------------ */

/* Cap per-trace FAIL detail; surplus fails counted but not printed.
   Mutable so -v can raise it to effectively unlimited. */
#define TRACE_FAIL_DETAIL_DEFAULT 3

static int trace_ok;
static _Atomic int trace_fail_count;   /* atomic: C traces increment from pthreads */
static int trace_fail_limit = TRACE_FAIL_DETAIL_DEFAULT;  /* -v bumps to INT_MAX */

/* Build the full "       -> FAIL ... \n" line into a local buffer, then emit
   with a single fputs so concurrent threads writing to the same FILE (the
   pipe back to the parent, since every trace runs in a forked child) can't
   interleave each other's lines.  Pipe writes of <= PIPE_BUF bytes are
   atomic on Linux; 512 bytes is well within that. */
#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            trace_ok = 0;                                                      \
            int _seq = atomic_fetch_add_explicit(&trace_fail_count, 1,         \
                                                 memory_order_relaxed);        \
            if (!quiet_mode && _seq < trace_fail_limit)                 \
            {                                                                  \
                char _lb[512];                                                 \
                int _k = snprintf(_lb, sizeof _lb,                             \
                                  "       -> FAIL [%s:%d]: ",                  \
                                  __FILE__, __LINE__);                         \
                if (_k < 0) _k = 0;                                            \
                if ((size_t)_k < sizeof _lb)                                   \
                    _k += snprintf(_lb + _k, sizeof _lb - (size_t)_k,          \
                                   __VA_ARGS__);                               \
                if (_k < 0 || (size_t)_k >= sizeof _lb - 1)                    \
                    _k = (int)sizeof _lb - 2;                                  \
                _lb[_k] = '\n';                                                \
                _lb[_k + 1] = '\0';                                            \
                fputs(_lb, stderr);                                            \
                fflush(stderr);                                                \
            }                                                                  \
        }                                                                      \
    } while (0)

static void trace_fail_tail(FILE *out)
{
    if (quiet_mode) return;
    int total = atomic_load_explicit(&trace_fail_count, memory_order_relaxed);
    int extra = total - trace_fail_limit;
    if (extra > 0)
    {
        fprintf(out, "       -> (... %d more fail%s suppressed)\n",
                extra, extra == 1 ? "" : "s");
        fflush(out);
    }
}

static size_t disk_size(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return (size_t)ps * 64;
}

static void compact_fsck_output(char out[])
{
    char *w = out;
    int in_space = 0;
    for (char *r = out; *r; r++)
    {
        unsigned char c = (unsigned char)*r;
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';

        if (c == ' ')
        {
            if (in_space)
                continue;
            in_space = 1;
        }
        else
        {
            in_space = 0;
        }
        *w++ = (char)c;
    }
    *w = '\0';
}

static int run_fsck_capture(const char *disk_name, char out[], size_t cap)
{
    if (cap > 0)
        out[0] = '\0';

    fflush(stdout);
    fflush(stderr);

    char path[] = "/tmp/sfslab-fsck.XXXXXX";
    int out_fd = mkstemp(path);
    if (out_fd < 0)
    {
        snprintf(out, cap, "mkstemp failed: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(out_fd);
        unlink(path);
        snprintf(out, cap, "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
        close(out_fd);
        execlp("timeout", "timeout", "5s", "./sfs-fsck", disk_name,
               (char *)NULL);
        _exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    for (;;)
    {
        pid_t r = waitpid(pid, &status, 0);
        if (r == pid)
            break;
        if (r < 0 && errno != EINTR)
        {
            close(out_fd);
            unlink(path);
            return -1;
        }
    }

    if (cap > 0)
    {
        if (lseek(out_fd, 0, SEEK_SET) >= 0)
        {
            ssize_t n = read(out_fd, out, cap - 1);
            if (n > 0)
                out[n] = '\0';
            else
                out[0] = '\0';
        }
    }
    close(out_fd);
    unlink(path);
    compact_fsck_output(out);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static void check_disk_consistency(const char *disk_name)
{
    char detail[384];
    int rc = run_fsck_capture(disk_name, detail, sizeof detail);
    if (rc == 0)
        return;
    if (detail[0] == '\0')
        snprintf(detail, sizeof detail, "no diagnostic output captured");
    CHECK(0, "sfs-fsck failed on %s (exit %d): %s", disk_name, rc, detail);
}

static void unmount_and_check(const char *disk_name)
{
    int r = sfs_unmount();
    CHECK(r == 0, "sfs_unmount returned %d", r);
    if (r == 0)
        check_disk_consistency(disk_name);
}

/* ================================================================== */
/*  Category A -- Feature Tests                                         */
/* ================================================================== */

/* A00: format, mount, unmount */
static int trace_A00(void)
{
    trace_ok = 1;
    int r = sfs_format(DISK_NAME, disk_size());
    CHECK(r == 0, "sfs_format returned %d", r);

    r = sfs_unmount();
    CHECK(r == 0, "sfs_unmount returned %d", r);
    if (r == 0)
        check_disk_consistency(DISK_NAME);

    r = sfs_mount(DISK_NAME);
    CHECK(r == 0, "sfs_mount returned %d", r);

    r = sfs_unmount();
    CHECK(r == 0, "sfs_unmount returned %d", r);
    if (r == 0)
        check_disk_consistency(DISK_NAME);
    return trace_ok;
}

/* A01: open, close, read, write */
static int trace_A01(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("hello.txt");
    CHECK(fd >= 0, "sfs_open returned %d", fd);

    const char *msg = "Hello, SFS!";
    ssize_t nw = sfs_write(fd, msg, strlen(msg));
    CHECK(nw == (ssize_t)strlen(msg), "sfs_write returned %zd", nw);
    sfs_close(fd);

    fd = sfs_open("hello.txt");
    CHECK(fd >= 0, "sfs_open (reopen) returned %d", fd);

    char buf[64] = {0};
    ssize_t nr = sfs_read(fd, buf, sizeof buf);
    CHECK(nr == (ssize_t)strlen(msg), "sfs_read returned %zd", nr);
    CHECK(memcmp(buf, msg, (size_t)nr) == 0, "data mismatch");
    sfs_close(fd);

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* A02: sfs_getpos */
static int trace_A02(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("pos.txt");
    CHECK(fd >= 0, "sfs_open returned %d", fd);

    ssize_t pos = sfs_getpos(fd);
    CHECK(pos == 0, "initial getpos should be 0, got %zd", pos);

    sfs_write(fd, "abcdefghij", 10);
    pos = sfs_getpos(fd);
    CHECK(pos == 10, "getpos after write(10) should be 10, got %zd", pos);

    CHECK(sfs_getpos(-1) == -EBADF, "getpos(-1) should return -EBADF");
    CHECK(sfs_getpos(99) == -EBADF, "getpos(99) should return -EBADF");

    sfs_close(fd);
    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* A03: sfs_seek */
static int trace_A03(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("seek.txt");
    sfs_write(fd, "0123456789", 10);

    ssize_t p = sfs_seek(fd, -5);
    CHECK(p == 5, "seek(-5) from 10 -> expected 5, got %zd", p);

    char buf[4] = {0};
    ssize_t nr = sfs_read(fd, buf, 3);
    CHECK(nr == 3 && memcmp(buf, "567", 3) == 0,
          "read after seek: got %zd bytes '%.*s'", nr, (int)nr, buf);

    p = sfs_seek(fd, -100);
    CHECK(p == 0, "seek past beginning should clamp to 0, got %zd", p);

    p = sfs_seek(fd, 9999);
    CHECK(p == 10, "seek past end should clamp to file size, got %zd", p);

    CHECK(sfs_seek(-1, 0) == -EBADF, "seek(-1,0) should return -EBADF");

    sfs_close(fd);
    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* A04: sfs_rename (basic + overwrite) */
static int trace_A04(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("old.txt");
    sfs_write(fd, "rename me", 9);
    sfs_close(fd);

    int r = sfs_rename("old.txt", "new.txt");
    CHECK(r == 0, "sfs_rename returned %d", r);

    r = sfs_unmount();
    CHECK(r == 0, "sfs_unmount returned %d", r);
    if (r == 0)
        check_disk_consistency(DISK_NAME);
    r = sfs_mount(DISK_NAME);
    CHECK(r == 0, "sfs_mount returned %d", r);

    fd = sfs_open("new.txt");
    CHECK(fd >= 0, "open new.txt after rename returned %d", fd);
    char buf[32] = {0};
    ssize_t nr = sfs_read(fd, buf, sizeof buf);
    CHECK(nr == 9 && memcmp(buf, "rename me", 9) == 0,
          "data after rename mismatch");
    sfs_close(fd);

    r = sfs_rename("nonexistent", "whatever");
    CHECK(r == -ENOENT, "rename nonexistent should be -ENOENT, got %d", r);

    /* overwrite rename */
    fd = sfs_open("a.txt");
    sfs_write(fd, "AAA", 3);
    sfs_close(fd);

    fd = sfs_open("b.txt");
    sfs_write(fd, "BBB", 3);
    sfs_close(fd);

    r = sfs_rename("a.txt", "b.txt");
    CHECK(r == 0, "rename a->b returned %d", r);

    fd = sfs_open("b.txt");
    CHECK(fd >= 0, "open b.txt after overwrite rename returned %d", fd);
    memset(buf, 0, sizeof buf);
    nr = sfs_read(fd, buf, sizeof buf);
    CHECK(nr == 3 && memcmp(buf, "AAA", 3) == 0,
          "b.txt should contain 'AAA', got '%.*s'", (int)nr, buf);
    sfs_close(fd);

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* ================================================================== */
/*  Category B -- Sequential Correctness                                */
/* ================================================================== */

/* B00: remove + list */
static int trace_B00(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("del.txt");
    sfs_write(fd, "bye", 3);
    sfs_close(fd);

    int r = sfs_remove("del.txt");
    CHECK(r == 0, "sfs_remove returned %d", r);
    r = sfs_remove("del.txt");
    CHECK(r == -ENOENT, "double remove should be -ENOENT, got %d", r);

    int fd1 = sfs_open("file1");
    int fd2 = sfs_open("file2");
    int fd3 = sfs_open("file3");
    CHECK(fd1 >= 0 && fd2 >= 0 && fd3 >= 0,
          "opening file1/file2/file3 returned %d/%d/%d", fd1, fd2, fd3);
    sfs_close(fd1);
    sfs_close(fd2);
    sfs_close(fd3);

    r = sfs_remove("file2");
    CHECK(r == 0, "remove file2 returned %d", r);
    int fd4 = sfs_open("file4");
    CHECK(fd4 >= 0, "open file4 after removing file2 returned %d", fd4);
    sfs_close(fd4);

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    int saw_del = 0;
    int saw_file1 = 0;
    int saw_file2 = 0;
    int saw_file3 = 0;
    int saw_file4 = 0;
    while (sfs_list(&cookie, name, sizeof name) == 0)
    {
        count++;
        if (strcmp(name, "del.txt") == 0) saw_del = 1;
        if (strcmp(name, "file1") == 0) saw_file1 = 1;
        if (strcmp(name, "file2") == 0) saw_file2 = 1;
        if (strcmp(name, "file3") == 0) saw_file3 = 1;
        if (strcmp(name, "file4") == 0) saw_file4 = 1;
    }
    CHECK(count == 3, "expected 3 files, got %d", count);
    CHECK(!saw_del && !saw_file2,
          "removed names should not appear in list");
    CHECK(saw_file1 && saw_file3 && saw_file4,
          "file1/file3/file4 should all appear after directory-slot reuse");

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* B01: multi-block file + seek across block boundaries
   BLOCK_DATA_SIZE = 500, so a 1200-byte file spans 3 blocks.
   This is the critical test for sfs_seek: the student must walk
   the block linked list when seeking across block boundaries. */
static int trace_B01(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    /* Build a 1200-byte payload: 'A' x 500 + 'B' x 500 + 'C' x 200 */
    char big[1200];
    memset(big, 'A', 500);
    memset(big + 500, 'B', 500);
    memset(big + 1000, 'C', 200);

    int fd = sfs_open("multi.txt");
    CHECK(fd >= 0, "open multi.txt returned %d", fd);

    ssize_t nw = sfs_write(fd, big, 1200);
    CHECK(nw == 1200, "write 1200 bytes returned %zd", nw);

    ssize_t pos = sfs_getpos(fd);
    CHECK(pos == 1200, "getpos after write(1200) should be 1200, got %zd", pos);

    /* Seek back to byte 490 (still in block 0, near boundary) */
    ssize_t p = sfs_seek(fd, -710);
    CHECK(p == 490, "seek(-710) from 1200 -> expected 490, got %zd", p);

    /* Read 20 bytes: should cross from block 0 into block 1 */
    char buf[64] = {0};
    ssize_t nr = sfs_read(fd, buf, 20);
    CHECK(nr == 20, "read(20) at pos 490 returned %zd", nr);
    /* bytes 490-499 are 'A', bytes 500-509 are 'B' */
    char expected[20];
    memset(expected, 'A', 10);
    memset(expected + 10, 'B', 10);
    CHECK(memcmp(buf, expected, 20) == 0,
          "cross-boundary read at 490: data mismatch");

    pos = sfs_getpos(fd);
    CHECK(pos == 510, "getpos after cross-boundary read should be 510, got %zd",
          pos);

    /* Seek to byte 999 (last byte of block 1), read into block 2 */
    p = sfs_seek(fd, 489);
    CHECK(p == 999, "seek(489) from 510 -> expected 999, got %zd", p);

    nr = sfs_read(fd, buf, 10);
    CHECK(nr == 10, "read(10) at pos 999 returned %zd", nr);
    /* byte 999 is 'B', bytes 1000-1008 are 'C' */
    CHECK(buf[0] == 'B' && buf[1] == 'C' && buf[9] == 'C',
          "cross-boundary read at 999: data mismatch");

    /* Seek round-trip: spec says sfs_seek(fd, loc - sfs_getpos(fd))
       should restore position */
    ssize_t saved = sfs_getpos(fd);
    sfs_seek(fd, 200);
    p = sfs_seek(fd, saved - sfs_getpos(fd));
    CHECK(p == saved,
          "seek round-trip: expected %zd, got %zd", saved, p);

    /* Seek all the way back to 0 from deep in the file */
    p = sfs_seek(fd, -9999);
    CHECK(p == 0, "seek to beginning should clamp to 0, got %zd", p);

    /* Read from beginning to verify block chain is intact */
    nr = sfs_read(fd, buf, 5);
    CHECK(nr == 5 && memcmp(buf, "AAAAA", 5) == 0,
          "read from beginning after seek-back: data mismatch");

    sfs_close(fd);

    /* Rename + remove workflow */
    int r = sfs_rename("multi.txt", "renamed.txt");
    CHECK(r == 0, "rename multi.txt returned %d", r);

    fd = sfs_open("renamed.txt");
    CHECK(fd >= 0, "open renamed.txt returned %d", fd);
    nr = sfs_read(fd, buf, 3);
    CHECK(nr == 3 && memcmp(buf, "AAA", 3) == 0,
          "renamed file data mismatch");
    sfs_close(fd);

    r = sfs_remove("renamed.txt");
    CHECK(r == 0, "remove renamed.txt returned %d", r);

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* B02: edge cases + getpos tracking + multi-fd independence */
static int trace_B02(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    /* read from empty file */
    int fd = sfs_open("empty.txt");
    char buf[64];
    ssize_t nr = sfs_read(fd, buf, sizeof buf);
    CHECK(nr == 0, "read from empty file should return 0, got %zd", nr);

    /* getpos on empty file */
    ssize_t pos = sfs_getpos(fd);
    CHECK(pos == 0, "getpos on empty file should be 0, got %zd", pos);

    /* seek on empty file */
    ssize_t p = sfs_seek(fd, 100);
    CHECK(p == 0, "seek(100) on empty file should clamp to 0, got %zd", p);
    p = sfs_seek(fd, -100);
    CHECK(p == 0, "seek(-100) on empty file should clamp to 0, got %zd", p);
    sfs_close(fd);

    CHECK(sfs_read(fd, buf, 1) == -EBADF,
          "read on closed fd should return -EBADF");
    CHECK(sfs_write(fd, "x", 1) == -EBADF,
          "write on closed fd should return -EBADF");
    CHECK(sfs_getpos(fd) == -EBADF,
          "getpos on closed fd should return -EBADF");
    CHECK(sfs_seek(fd, 0) == -EBADF,
          "seek on closed fd should return -EBADF");
    sfs_close(fd);

    CHECK(sfs_read(-1, buf, 1) == -EBADF,
          "read(-1) should return -EBADF");
    CHECK(sfs_write(-1, "x", 1) == -EBADF,
          "write(-1) should return -EBADF");
    CHECK(sfs_read(32, buf, 1) == -EBADF,
          "read(32) should return -EBADF");
    CHECK(sfs_write(32, "x", 1) == -EBADF,
          "write(32) should return -EBADF");
    sfs_close(32);

    /* name too long */
    char longname[SFS_FILE_NAME_SIZE_LIMIT + 8];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    int r = sfs_open(longname);
    CHECK(r == -ENAMETOOLONG, "open(longname) should be -ENAMETOOLONG, got %d",
          r);

    /* getpos tracks reads correctly */
    fd = sfs_open("track.txt");
    sfs_write(fd, "0123456789abcdef", 16);
    sfs_close(fd);
    fd = sfs_open("track.txt");
    nr = sfs_read(fd, buf, 7);
    CHECK(nr == 7, "read(7) returned %zd", nr);
    pos = sfs_getpos(fd);
    CHECK(pos == 7, "getpos after read(7) should be 7, got %zd", pos);
    nr = sfs_read(fd, buf, 3);
    CHECK(nr == 3, "read(3) returned %zd", nr);
    pos = sfs_getpos(fd);
    CHECK(pos == 10, "getpos after read(7)+read(3) should be 10, got %zd", pos);
    CHECK(memcmp(buf, "789", 3) == 0, "read(3) data mismatch");
    sfs_close(fd);

    /* double open: independent positions */
    fd = sfs_open("dup.txt");
    sfs_write(fd, "hello world!", 12);
    int fd2 = sfs_open("dup.txt");
    CHECK(fd2 >= 0 && fd2 != fd, "second open should give different fd");

    nr = sfs_read(fd2, buf, 5);
    CHECK(nr == 5 && memcmp(buf, "hello", 5) == 0,
          "second fd should read 'hello'");
    pos = sfs_getpos(fd2);
    CHECK(pos == 5, "fd2 getpos should be 5, got %zd", pos);

    /* fd1 is at pos 12 (after write), fd2 is at pos 5 -- independent */
    pos = sfs_getpos(fd);
    CHECK(pos == 12, "fd1 getpos should still be 12, got %zd", pos);

    sfs_close(fd);
    sfs_close(fd2);

    /* open-file lifecycle: remove and unmount must respect live fds */
    fd = sfs_open("busy.txt");
    CHECK(fd >= 0, "open busy.txt returned %d", fd);
    CHECK(sfs_write(fd, "busy", 4) == 4, "write busy.txt failed");
    r = sfs_remove("busy.txt");
    CHECK(r == -EBUSY, "remove(open file) should be -EBUSY, got %d", r);
    r = sfs_unmount();
    CHECK(r == -EBUSY, "unmount with open file should be -EBUSY, got %d", r);
    sfs_close(fd);
    r = sfs_remove("busy.txt");
    CHECK(r == 0, "remove busy.txt after close returned %d", r);

    /* write exactly BLOCK_DATA_SIZE bytes (boundary condition) */
    fd = sfs_open("boundary.txt");
    char fill[500];
    memset(fill, 'Z', 500);
    ssize_t nw = sfs_write(fd, fill, 500);
    CHECK(nw == 500, "write(500) returned %zd", nw);
    pos = sfs_getpos(fd);
    CHECK(pos == 500, "getpos after write(500) should be 500, got %zd", pos);
    p = sfs_seek(fd, -500);
    CHECK(p == 0, "seek(-500) from 500 should be 0, got %zd", p);
    nr = sfs_read(fd, buf, 3);
    CHECK(nr == 3 && memcmp(buf, "ZZZ", 3) == 0,
          "read after seek to 0: data mismatch");
    sfs_close(fd);

    /* fd-table exhaustion.  The handout's OPEN_FILE_LIMIT (sfs-disk.c)
       is 32, but sfs-api.h does not pin the table size, so enlarging it
       is a legitimate improvement.  The contract tested here is: at
       least 32 simultaneous opens of one file succeed, and running out
       of descriptors -- if it ever happens -- is reported as -EMFILE.
       (The handout hits that at open #33.) */
    enum { FD_PROBE_MAX = 64 };
    int fds[FD_PROBE_MAX];
    int n_open = 0;
    int probe_err = 0;
    for (int i = 0; i < FD_PROBE_MAX; i++)
    {
        int pfd = sfs_open("many");
        if (pfd < 0)
        {
            probe_err = pfd;
            break;
        }
        fds[n_open++] = pfd;
    }
    CHECK(n_open >= 32,
          "at least 32 simultaneous opens should succeed; open #%d "
          "failed with %d", n_open + 1, probe_err);
    CHECK(n_open == FD_PROBE_MAX || probe_err == -EMFILE,
          "running out of descriptors should report -EMFILE, got %d",
          probe_err);
    for (int i = 0; i < n_open; i++)
        sfs_close(fds[i]);
    r = sfs_remove("many");
    CHECK(r == 0, "remove(\"many\") after closing all fds returned %d", r);

    unmount_and_check(DISK_NAME);

    /* disk-space exhaustion, on the smallest image sfs_format accepts:
       one page holds pagesize/512 blocks, one of which is the superblock.
       A file's first block comes with creation; each further 500-byte
       write allocates exactly one more. */
    size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
    size_t data_blocks = pagesz / 512 - 1;
    r = sfs_format(DISK_NAME, pagesz);
    CHECK(r == 0, "sfs_format(one page) returned %d", r);

    fd = sfs_open("big");
    CHECK(fd >= 0, "open(\"big\") on the tiny disk returned %d", fd);
    for (size_t b = 0; b < data_blocks; b++)
    {
        nw = sfs_write(fd, fill, 500);
        CHECK(nw == 500, "write filling block %zu of %zu returned %zd",
              b + 1, data_blocks, nw);
    }
    nw = sfs_write(fd, fill, 500);
    CHECK(nw == -ENOSPC, "write on a full disk should be -ENOSPC, got %zd",
          nw);
    int fdfull = sfs_open("nofit");
    CHECK(fdfull == -ENOSPC,
          "creating a file on a full disk should be -ENOSPC, got %d",
          fdfull);
    sfs_close(fd);

    /* removing the file frees its blocks again */
    r = sfs_remove("big");
    CHECK(r == 0, "remove(\"big\") returned %d", r);
    fdfull = sfs_open("fits");
    CHECK(fdfull >= 0, "open after freeing space returned %d", fdfull);
    CHECK(sfs_write(fdfull, "ok", 2) == 2, "write after freeing space failed");
    sfs_close(fdfull);
    CHECK(sfs_remove("fits") == 0, "remove(\"fits\") returned nonzero");

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* B03: rename/list edge cases */
static int trace_B03(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("src");
    CHECK(fd >= 0, "open src returned %d", fd);
    CHECK(sfs_write(fd, "SRC-DATA", 8) == 8, "write src failed");
    sfs_close(fd);

    fd = sfs_open("dst");
    CHECK(fd >= 0, "open dst returned %d", fd);
    CHECK(sfs_write(fd, "D", 1) == 1, "write dst failed");
    sfs_close(fd);

    fd = sfs_open("keep");
    CHECK(fd >= 0, "open keep returned %d", fd);
    sfs_close(fd);

    int r = sfs_rename("src", "dst");
    CHECK(r == 0, "rename src->dst returned %d", r);

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    int saw_src = 0;
    int saw_dst = 0;
    int saw_keep = 0;
    while (sfs_list(&cookie, name, sizeof name) == 0)
    {
        count++;
        if (strcmp(name, "src") == 0) saw_src = 1;
        if (strcmp(name, "dst") == 0) saw_dst = 1;
        if (strcmp(name, "keep") == 0) saw_keep = 1;
    }
    CHECK(count == 2, "expected 2 files after overwrite rename, got %d",
          count);
    CHECK(!saw_src, "src should not remain after rename");
    CHECK(saw_dst && saw_keep, "dst and keep should be listed");

    fd = sfs_open("dst");
    CHECK(fd >= 0, "open dst after rename returned %d", fd);
    char buf[16] = {0};
    ssize_t nr = sfs_read(fd, buf, sizeof buf);
    CHECK(nr == 8 && memcmp(buf, "SRC-DATA", 8) == 0,
          "dst should contain renamed src data with updated size");
    sfs_close(fd);

    fd = sfs_open("reuse");
    CHECK(fd >= 0, "open reuse after overwrite rename returned %d", fd);
    CHECK(sfs_write(fd, "R", 1) == 1, "write reuse failed");
    sfs_close(fd);

    cookie = NULL;
    count = 0;
    saw_src = 0;
    saw_dst = 0;
    saw_keep = 0;
    int saw_reuse = 0;
    while (sfs_list(&cookie, name, sizeof name) == 0)
    {
        count++;
        if (strcmp(name, "src") == 0) saw_src = 1;
        if (strcmp(name, "dst") == 0) saw_dst = 1;
        if (strcmp(name, "keep") == 0) saw_keep = 1;
        if (strcmp(name, "reuse") == 0) saw_reuse = 1;
    }
    CHECK(count == 3, "expected 3 files after rename-slot reuse, got %d",
          count);
    CHECK(!saw_src, "src should not reappear after slot reuse");
    CHECK(saw_dst && saw_keep && saw_reuse,
          "dst, keep, and reuse should be listed");

    char longname[SFS_FILE_NAME_SIZE_LIMIT + 8];
    memset(longname, 'x', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    CHECK(sfs_remove(longname) == -ENAMETOOLONG,
          "remove(longname) should return -ENAMETOOLONG");
    CHECK(sfs_rename("dst", longname) == -ENAMETOOLONG,
          "rename to longname should return -ENAMETOOLONG");

    cookie = NULL;
    CHECK(sfs_list(&cookie, name, 0) == -EINVAL,
          "list with zero filename_space should return -EINVAL");

    /* sfs-api.h: whenever sfs_list returns nonzero -- errors included --
       it also resets the cookie to NULL.  Exercise that from a *live*
       cookie: one successful call first, then force an error. */
    cookie = NULL;
    r = sfs_list(&cookie, name, sizeof name);
    CHECK(r == 0, "list first call should return a name, got %d", r);
    CHECK(cookie != NULL, "cookie should be non-NULL mid-iteration");
    r = sfs_list(&cookie, name, 0);
    CHECK(r == -EINVAL, "mid-iteration error should be -EINVAL, got %d", r);
    CHECK(cookie == NULL,
          "cookie must be reset to NULL on a nonzero return (still %p)",
          cookie);

    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* ================================================================== */
/*  Category C -- Concurrent Correctness                                */
/* ================================================================== */

#define NUM_THREADS 4

/* ------------------------------------------------------------------ */
/*  Seeded schedule fuzz                                               */
/*                                                                     */
/*  When sched_fuzz_seed is nonzero, fuzz_point() randomly yields or   */
/*  briefly sleeps around SFS API calls, so concurrent traces explore  */
/*  more interleavings than the natural scheduler produces.  The PRNG  */
/*  is deterministic per (seed, thread), so a failure reproduces with  */
/*  the same --sched-fuzz=SEED.  Fuzzing only perturbs timing: any     */
/*  race or trace failure it surfaces is real.                         */
/* ------------------------------------------------------------------ */

static unsigned int sched_fuzz_seed; /* 0 = fuzz off */
static _Atomic unsigned int fuzz_thread_counter;
static _Thread_local uint64_t fuzz_state;

static void fuzz_point(void)
{
    if (sched_fuzz_seed == 0)
        return;
    if (fuzz_state == 0)
    {
        unsigned int tid = atomic_fetch_add_explicit(
            &fuzz_thread_counter, 1, memory_order_relaxed);
        fuzz_state = ((uint64_t)sched_fuzz_seed + 1) * 0x9E3779B97F4A7C15ull +
                     ((uint64_t)tid + 1) * 0xBF58476D1CE4E5B9ull;
    }
    /* xorshift64* */
    fuzz_state ^= fuzz_state >> 12;
    fuzz_state ^= fuzz_state << 25;
    fuzz_state ^= fuzz_state >> 27;
    uint64_t r = fuzz_state * 0x2545F4914F6CDD1Dull;

    unsigned int pick = (unsigned int)(r % 8u);
    if (pick < 4)
        return; /* half the time: run through undisturbed */
    if (pick < 7)
    {
        sched_yield();
        return;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)(r % 50u) * 1000L};
    nanosleep(&ts, NULL); /* 0-49 microseconds */
}

static pthread_mutex_t c_api_mutex = PTHREAD_MUTEX_INITIALIZER;
static int serialize_c_api_calls = 1;

/* Normal C traces are a stable functional signal.  They serialize individual
   API calls so the unfinished starter does not get schedule-dependent scores.
   The --tsan-only path disables this guard and exercises real concurrent calls
   for race detection after A/B/C correctness is otherwise complete. */
static void c_api_enter(void)
{
    fuzz_point();
    if (serialize_c_api_calls)
        pthread_mutex_lock(&c_api_mutex);
}

static void c_api_leave(void)
{
    if (serialize_c_api_calls)
        pthread_mutex_unlock(&c_api_mutex);
    fuzz_point();
}

static int c_sfs_open(const char *fileName)
{
    c_api_enter();
    int ret = sfs_open(fileName);
    c_api_leave();
    return ret;
}

static void c_sfs_close(int fd)
{
    c_api_enter();
    sfs_close(fd);
    c_api_leave();
}

static ssize_t c_sfs_read(int fd, char *buf, size_t len)
{
    c_api_enter();
    ssize_t ret = sfs_read(fd, buf, len);
    c_api_leave();
    return ret;
}

static ssize_t c_sfs_write(int fd, const char *buf, size_t len)
{
    c_api_enter();
    ssize_t ret = sfs_write(fd, buf, len);
    c_api_leave();
    return ret;
}

static int c_sfs_list(sfs_list_cookie *cookie, char filename_out[],
                      size_t filename_space)
{
    c_api_enter();
    int ret = sfs_list(cookie, filename_out, filename_space);
    c_api_leave();
    return ret;
}

static int c_sfs_remove(const char *name)
{
    c_api_enter();
    int ret = sfs_remove(name);
    c_api_leave();
    return ret;
}

static int c_sfs_rename(const char *old_name, const char *new_name)
{
    c_api_enter();
    int ret = sfs_rename(old_name, new_name);
    c_api_leave();
    return ret;
}

static void *thread_write_own_file(void *arg)
{
    int id = *(int *)arg;
    char fname[SFS_FILE_NAME_SIZE_LIMIT];
    snprintf(fname, sizeof fname, "thr%d.txt", id);

    int fd = c_sfs_open(fname);
    if (fd < 0)
        return (void *)(intptr_t)fd;

    char data[64];
    int len = snprintf(data, sizeof data, "thread-%d-payload", id);
    c_sfs_write(fd, data, (size_t)len);
    c_sfs_close(fd);

    fd = c_sfs_open(fname);
    if (fd < 0)
        return (void *)(intptr_t)fd;

    char buf[64] = {0};
    ssize_t nr = c_sfs_read(fd, buf, sizeof buf);
    c_sfs_close(fd);

    if (nr != len || memcmp(buf, data, (size_t)len) != 0)
        return (void *)(intptr_t)-1;
    return NULL;
}

/* C00: concurrent writes to separate files */
static int trace_C00(void)
{
    trace_ok = 1;
    sfs_format(CONC_DISK_C00, disk_size());

    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, thread_write_own_file, &ids[i]);
    }

    int ok = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret;
        pthread_join(threads[i], &ret);
        if (ret != NULL)
            ok = 0;
    }
    CHECK(ok, "concurrent writes to separate files: data corruption");

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    while (c_sfs_list(&cookie, name, sizeof name) == 0)
        count++;
    CHECK(count == NUM_THREADS, "expected %d files, got %d", NUM_THREADS,
          count);

    unmount_and_check(CONC_DISK_C00);
    unlink(CONC_DISK_C00);
    return trace_ok;
}

static void *thread_read_shared(void *arg)
{
    int fd = c_sfs_open("shared.txt");
    if (fd < 0)
        return (void *)(intptr_t)fd;

    char buf[64] = {0};
    ssize_t nr = c_sfs_read(fd, buf, sizeof buf);
    c_sfs_close(fd);

    if (nr != 6 || memcmp(buf, "SHARED", 6) != 0)
        return (void *)(intptr_t)-1;
    return NULL;
}

/* C01: concurrent reads of same file */
static int trace_C01(void)
{
    trace_ok = 1;
    sfs_format(CONC_DISK_C01, disk_size());

    int fd = c_sfs_open("shared.txt");
    c_sfs_write(fd, "SHARED", 6);
    c_sfs_close(fd);

    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_read_shared, NULL);

    int ok = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret;
        pthread_join(threads[i], &ret);
        if (ret != NULL)
            ok = 0;
    }
    CHECK(ok, "concurrent reads of same file failed");

    unmount_and_check(CONC_DISK_C01);
    unlink(CONC_DISK_C01);
    return trace_ok;
}

struct rw_mix_arg
{
    int id;
    int do_write;
};

static void *thread_rw_mix(void *arg)
{
    struct rw_mix_arg *a = arg;
    char fname[SFS_FILE_NAME_SIZE_LIMIT];
    snprintf(fname, sizeof fname, "mix%d.txt", a->id);

    if (a->do_write)
    {
        int fd = c_sfs_open(fname);
        if (fd < 0)
            return (void *)(intptr_t)-1;
        char data[32];
        int len = snprintf(data, sizeof data, "data-%d", a->id);
        c_sfs_write(fd, data, (size_t)len);
        c_sfs_close(fd);
    }
    else
    {
        int fd = c_sfs_open(fname);
        if (fd < 0)
            return (void *)(intptr_t)-1;
        char buf[32];
        c_sfs_read(fd, buf, sizeof buf);
        c_sfs_close(fd);
    }
    return NULL;
}

static void *thread_open_close_storm(void *arg)
{
    for (int i = 0; i < 20; i++)
    {
        int fd = c_sfs_open("storm.txt");
        if (fd >= 0)
            c_sfs_close(fd);
    }
    return NULL;
}

/* Part 3 of C02: directory churn.  Each churn thread repeatedly creates
   its own file and then renames it onto a per-thread destination, so
   iterations after the first exercise overwrite-rename while the lister
   below walks the directory.  While sfs_rename is still the -ENOSYS
   starter stub, the thread falls back to a plain remove, so the trace is
   meaningful (and passes) before the stub is implemented.  All names are
   per-thread, which keeps the serialized run deterministic; under the
   TSan rerun this same code makes real concurrent create/rename/remove
   calls. */
#define CHURN_ITERS 12

static _Atomic int churn_done;

static void *thread_dir_churn(void *arg)
{
    int id = *(int *)arg;
    char dst[SFS_FILE_NAME_SIZE_LIMIT];
    snprintf(dst, sizeof dst, "stable%d", id);
    int renamed = 0;

    for (int i = 0; i < CHURN_ITERS; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        snprintf(name, sizeof name, "churn%d_%02d", id, i);

        int fd = c_sfs_open(name);
        if (fd < 0)
            return (void *)(intptr_t)-1;
        char data[32];
        int len = snprintf(data, sizeof data, "c-%d-%d", id, i);
        if (len < 0 || c_sfs_write(fd, data, (size_t)len) != (ssize_t)len)
        {
            c_sfs_close(fd);
            return (void *)(intptr_t)-1;
        }
        c_sfs_close(fd);

        int r = c_sfs_rename(name, dst);
        if (r == 0)
            renamed = 1;
        else if (r == -ENOSYS)
        {
            if (c_sfs_remove(name) != 0)
                return (void *)(intptr_t)-1;
        }
        else
            return (void *)(intptr_t)-1;
    }
    if (renamed && c_sfs_remove(dst) != 0)
        return (void *)(intptr_t)-1;
    return NULL;
}

static void *thread_list_during_churn(void *arg)
{
    int passes = 0;
    do
    {
        sfs_list_cookie cookie = NULL;
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        int status;
        while ((status = c_sfs_list(&cookie, name, sizeof name)) == 0)
        {
            if (name[0] == '\0')
                return (void *)(intptr_t)-1;
        }
        if (status != 1)
            return (void *)(intptr_t)-1;
        passes++;
        sched_yield();
    } while (!atomic_load_explicit(&churn_done, memory_order_acquire) ||
             passes < 4);
    return NULL;
}

/* C02: mixed r/w + open/close storm + directory churn */
static int trace_C02(void)
{
    trace_ok = 1;

    /* Part 1: r/w mix on separate files */
    sfs_format(CONC_DISK_C02_RW, disk_size());

    struct rw_mix_arg args[NUM_THREADS];
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
    {
        args[i].id = i;
        args[i].do_write = (i % 2 == 0);
        pthread_create(&threads[i], NULL, thread_rw_mix, &args[i]);
    }

    int ok = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret;
        pthread_join(threads[i], &ret);
        if (ret != NULL)
            ok = 0;
    }
    CHECK(ok, "concurrent r/w mix failed");
    unmount_and_check(CONC_DISK_C02_RW);
    unlink(CONC_DISK_C02_RW);

    /* Part 2: open/close storm on same file */
    sfs_format(CONC_DISK_C02_STORM, disk_size());

    int fd = c_sfs_open("storm.txt");
    c_sfs_write(fd, "x", 1);
    c_sfs_close(fd);

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_create(&threads[i], NULL, thread_open_close_storm, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    while (c_sfs_list(&cookie, name, sizeof name) == 0)
        count++;
    CHECK(count == 1, "storm: expected 1 file, got %d", count);

    unmount_and_check(CONC_DISK_C02_STORM);
    unlink(CONC_DISK_C02_STORM);

    /* Part 3: directory churn (create/rename/remove) under a live lister */
    sfs_format(CONC_DISK_C02_DIR, disk_size());
    atomic_store_explicit(&churn_done, 0, memory_order_release);

    pthread_t lister;
    int churn_ids[NUM_THREADS];
    pthread_create(&lister, NULL, thread_list_during_churn, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
    {
        churn_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_dir_churn, &churn_ids[i]);
    }

    ok = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret;
        pthread_join(threads[i], &ret);
        if (ret != NULL)
            ok = 0;
    }
    atomic_store_explicit(&churn_done, 1, memory_order_release);
    void *list_ret;
    pthread_join(lister, &list_ret);
    CHECK(ok, "directory churn (create/rename/remove) failed");
    CHECK(list_ret == NULL, "listing during directory churn failed");

    cookie = NULL;
    count = 0;
    while (c_sfs_list(&cookie, name, sizeof name) == 0)
        count++;
    CHECK(count == 0, "churn should leave no files, found %d", count);

    unmount_and_check(CONC_DISK_C02_DIR);
    unlink(CONC_DISK_C02_DIR);
    return trace_ok;
}

/* ================================================================== */
/*  Performance Benchmark                                              */
/* ================================================================== */

#define PERF_THREADS 8

/* Workload shape: each thread runs PERF_OUTER_ITERS sessions against
   its own file; a session is one open, then PERF_IO_ROUNDS rounds of
   write/seek/getpos/read, then one close.  Amortizing open/close over
   the I/O rounds keeps the (necessarily shared) fd table from
   dominating, so the measurement reflects how well *file data* ops on
   different files proceed in parallel -- the thing the lab asks you
   to make scale.  One "op" in all reported numbers is one SFS API
   call.

   Bump PERF_WORKLOAD_VERSION whenever the workload changes shape or
   size: .perf_baseline files record it (see the `baseline` target in
   the Makefile, which must stay in sync), and a baseline calibrated
   on a different workload is rejected rather than silently producing
   a meaningless ratio. */
#define PERF_OUTER_ITERS 2000
#define PERF_IO_ROUNDS 10
#define PERF_CALLS_PER_THREAD (PERF_OUTER_ITERS * (2 + 4 * PERF_IO_ROUNDS))
#define PERF_WORKLOAD_VERSION "v2"

/* Scored perf benchmark samples this many times and uses the median.
   Matches baseline calibration (make baseline BASELINE_RUNS=N); odd so
   the median is a single observed value. Raise if your environment is
   noisy (Docker bind mounts, shared laptops). */
#define PERF_SAMPLE_RUNS 5

static pthread_barrier_t perf_barrier;

/* Set by any perf worker that observes a wrong return value or wrong
   bytes.  The scored benchmark refuses to award points once this fires,
   so an implementation that shortcuts the perf path (right speed, wrong
   results) earns 0/10 rather than a ratio. */
static _Atomic int perf_worker_failed;

static void *perf_worker(void *arg)
{
    int id = *(int *)arg;
    char fname[SFS_FILE_NAME_SIZE_LIMIT];
    snprintf(fname, sizeof fname, "perf%d.txt", id);

    pthread_barrier_wait(&perf_barrier);
    for (int i = 0; i < PERF_OUTER_ITERS; i++)
    {
        int fd = sfs_open(fname);
        if (fd < 0)
        {
            atomic_store_explicit(&perf_worker_failed, 1,
                                  memory_order_relaxed);
            continue;
        }
        ssize_t prev_nr = 0;
        for (int k = 0; k < PERF_IO_ROUNDS; k++)
        {
            char data[64];
            int len = snprintf(data, sizeof data, "iter-%d-%d-%d", id, i, k);
            ssize_t nw = sfs_write(fd, data, (size_t)len);
            ssize_t pos = sfs_getpos(fd);
            ssize_t sk = sfs_seek(fd, -pos);
            char buf[64];
            ssize_t nr = sfs_read(fd, buf, sizeof buf);

            /* Validate as we go.  Each round: the write lands at the
               position the previous read left (0 for round 0, since
               open resets the position), the seek returns to 0, and
               the read must cover at least the bytes just written.
               Round 0 wrote at offset 0, so there the read-back bytes
               are compared too.  The calibration baseline's getpos and
               seek are -ENOSYS stubs; position checks are skipped in
               that case, which cannot happen in a scored run because
               correctness (A02/A03) gates the benchmark. */
            if (nw != (ssize_t)len)
                goto bad;
            if (pos != -ENOSYS)
            {
                if (pos != prev_nr + nw || sk != 0 || nr < nw)
                    goto bad;
                if (k == 0 && memcmp(buf, data, (size_t)len) != 0)
                    goto bad;
                prev_nr = nr;
            }
        }
        sfs_close(fd);
        continue;
    bad:
        atomic_store_explicit(&perf_worker_failed, 1, memory_order_relaxed);
        sfs_close(fd);
    }
    return NULL;
}

static double elapsed_sec(struct timespec *start, struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

struct baseline_info
{
    double ops;
    time_t mtime;                  /* 0 if unavailable */
    char   path[PATH_MAX];
    char   disk_dir[PATH_MAX];     /* SFS_DISK_DIR at calibration time; "(unset)" if absent */
    char   workload[32];           /* WORKLOAD= tag; "v1" for pre-tag files */
};

/* Populate `info` from ./.perf_baseline.  Like the rest of the driver
   (sfs-fsck invocation, TSan self-compile), this expects to run from the
   handout directory.  Returns 0 on success, -1 if the file is missing,
   unreadable, or holds a non-finite / non-positive value. */
static int load_baseline(struct baseline_info *info)
{
    info->ops = -1.0;
    info->mtime = 0;
    snprintf(info->path, sizeof(info->path), ".perf_baseline");
    snprintf(info->disk_dir, sizeof(info->disk_dir), "(unset)");
    /* Files written before the workload tag existed are all from the
       original (v1) benchmark shape. */
    snprintf(info->workload, sizeof(info->workload), "v1");

    FILE *f = fopen(info->path, "r");
    if (!f) return -1;

    /* .perf_baseline format:
         line 1: ops/sec as a decimal number
         further lines (optional "KEY=value", order-independent):
           "SFS_DISK_DIR=<value>"   (added 2026-04)
           "WORKLOAD=<version>"     (added 2026-06; absent means v1)
       Old files with fewer lines still parse. */
    char line[PATH_MAX + 64];
    double ops = -1.0;
    if (fgets(line, sizeof(line), f) == NULL ||
        sscanf(line, "%lf", &ops) != 1)
    {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = '\0';
        const char *dir_prefix = "SFS_DISK_DIR=";
        const char *wl_prefix = "WORKLOAD=";
        if (strncmp(line, dir_prefix, strlen(dir_prefix)) == 0)
        {
            const char *value = line + strlen(dir_prefix);
            size_t n = strlen(value);
            if (n >= sizeof(info->disk_dir))
                n = sizeof(info->disk_dir) - 1;
            memcpy(info->disk_dir, value, n);
            info->disk_dir[n] = '\0';
        }
        else if (strncmp(line, wl_prefix, strlen(wl_prefix)) == 0)
        {
            const char *value = line + strlen(wl_prefix);
            size_t n = strlen(value);
            if (n >= sizeof(info->workload))
                n = sizeof(info->workload) - 1;
            memcpy(info->workload, value, n);
            info->workload[n] = '\0';
        }
    }
    fclose(f);
    if (!isfinite(ops) || ops <= 0.0) return -1;

    struct stat st;
    if (stat(info->path, &st) == 0) info->mtime = st.st_mtime;

    info->ops = ops;
    return 0;
}

#define BASELINE_STALE_DAYS 30

/* Fallback scoring when no comparable baseline exists.  Absolute
   throughput cannot show *scaling* over a global-mutex implementation
   (a coarse-locked solution is fast in absolute terms on any modern
   machine), so this path tops out at 5/10; the remaining points
   require a calibrated ratio. */
static int score_perf_absolute(double student_ops)
{
    printf("  Scoring on absolute throughput is capped at 5/10; run "
           "`make baseline`\n"
           "  (or `make grade`) so scaling can be measured for full "
           "credit.\n");
    if (student_ops < 50000)   return 0;
    if (student_ops < 1000000) return 3;
    return 5;
}

/* Score student's ops/sec against the machine-calibrated baseline.
   ratio = student_ops / baseline_ops, where the baseline binary is the
   handout implementation under a single global mutex.  The ladder is
   calibrated (docs/maintainer/PERF_CALIBRATION.md) so that a correct
   solution which keeps one global lock measures ~1.0x and earns 3/10,
   while the scaling a per-file locking scheme delivers (>= ~4.5x on
   the calibration machine) clears the 10/10 bar with a wide margin. */
static int score_perf_against_baseline(double student_ops)
{
    struct baseline_info bi;
    if (load_baseline(&bi) != 0)
    {
        printf("  (.perf_baseline missing)\n");
        return score_perf_absolute(student_ops);
    }

    if (strcmp(bi.workload, PERF_WORKLOAD_VERSION) != 0)
    {
        printf("  (.perf_baseline was calibrated with grader workload %s, "
               "but this grader\n   measures workload %s -- the ratio "
               "would be meaningless.  Re-run `make baseline`.)\n",
               bi.workload, PERF_WORKLOAD_VERSION);
        return score_perf_absolute(student_ops);
    }

    double ratio = student_ops / bi.ops;
    printf("  Baseline throughput: %.0f ops/sec (from %s)\n",
           bi.ops, bi.path);
    if (bi.mtime > 0)
    {
        double age_days = difftime(time(NULL), bi.mtime) / 86400.0;
        if (age_days >= (double)BASELINE_STALE_DAYS)
            printf("  Baseline age: %.0f days -- stale, "
                   "re-run `make baseline` for a fair comparison\n",
                   age_days);
        else
            printf("  Baseline age: %.1f days\n", age_days);
    }

    /* Guard against baseline / scored-run environment mismatch: if the
       calibration ran with a different SFS_DISK_DIR, the numerator and
       denominator are measuring different filesystems and the ratio lies. */
    const char *cur_dir = getenv("SFS_DISK_DIR");
    if (!cur_dir || !*cur_dir) cur_dir = "(unset)";
    if (strcmp(cur_dir, bi.disk_dir) != 0)
    {
        printf("  WARNING: SFS_DISK_DIR differs between calibration and "
               "scored run:\n");
        printf("    baseline calibrated with SFS_DISK_DIR=%s\n", bi.disk_dir);
        printf("    scored run uses          SFS_DISK_DIR=%s\n", cur_dir);
        printf("    Ratio below is unreliable -- recalibrate with "
               "`make baseline`\n");
        printf("    using the same SFS_DISK_DIR value, or unset both.\n");
    }

    printf("  Ratio (student / baseline): %.2fx\n", ratio);

    if (ratio >= 2.50) return 10;  /* scaling only fine-grained locking reaches */
    if (ratio >= 1.80) return 9;
    if (ratio >= 1.40) return 7;
    if (ratio >= 1.20) return 5;
    if (ratio >= 0.85) return 3;   /* correct but coarse: ~the baseline itself */
    return 0;                      /* slower than the trivial global mutex */
}

static double run_perf_benchmark_raw(void)
{
    sfs_format(PERF_DISK, disk_size());

    pthread_barrier_init(&perf_barrier, NULL, PERF_THREADS + 1);

    pthread_t threads[PERF_THREADS];
    int ids[PERF_THREADS];
    for (int i = 0; i < PERF_THREADS; i++)
    {
        ids[i] = i;
        pthread_create(&threads[i], NULL, perf_worker, &ids[i]);
    }

    /* The barrier releases the workers only after all of them exist,
       so thread spawn cost stays out of the measurement.  (Spawning 8
       threads costs ~200us; the old un-barriered 100-iteration
       workload finished in about that long, which made the reported
       number mostly scheduler noise.) */
    struct timespec t0, t1;
    pthread_barrier_wait(&perf_barrier);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < PERF_THREADS; i++)
        pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_barrier_destroy(&perf_barrier);

    sfs_unmount();
    unlink(PERF_DISK);

    double secs = elapsed_sec(&t0, &t1);
    double total_calls = (double)PERF_THREADS * PERF_CALLS_PER_THREAD;
    return total_calls / secs;
}

static int cmp_double_asc(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static int run_perf_benchmark(void)
{
    double samples[PERF_SAMPLE_RUNS];
    for (int i = 0; i < PERF_SAMPLE_RUNS; i++)
    {
        samples[i] = run_perf_benchmark_raw();
        printf("  run %d/%d: %.0f ops/sec\n", i + 1, PERF_SAMPLE_RUNS,
               samples[i]);
        fflush(stdout);
    }

    if (atomic_load_explicit(&perf_worker_failed, memory_order_relaxed))
    {
        printf("  Perf workload returned wrong results (bad return value, "
               "position, or data\n  mismatch during the benchmark) -- "
               "performance score set to 0.\n");
        return 0;
    }

    qsort(samples, PERF_SAMPLE_RUNS, sizeof samples[0], cmp_double_asc);
    double median = samples[PERF_SAMPLE_RUNS / 2];
    double lo = samples[0];
    double hi = samples[PERF_SAMPLE_RUNS - 1];
    double spread = 200.0 * (hi - lo) / (hi + lo);

    long total_calls = (long)PERF_THREADS * PERF_CALLS_PER_THREAD;
    printf("  Student throughput (median of %d): %.0f ops/sec "
           "(%ld API calls/run, spread %.1f%%)\n",
           PERF_SAMPLE_RUNS, median, total_calls, spread);
    if (spread > 20.0)
        printf("  Warning: spread exceeds 20%% -- perf ratio will be noisy;\n"
               "           see the writeup's Docker/WSL notes for mitigation.\n");
    return score_perf_against_baseline(median);
}

/* ================================================================== */
/*  ThreadSanitizer race detection (auto-compiled and run)             */
/* ================================================================== */

/* Scan a (small) log file for NEEDLE; returns 1 if any line contains it. */
static int log_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f))
    {
        if (strstr(line, needle))
        {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* The TSan rerun sweeps this many schedule-fuzz seeds.  TSan only reports
   races that actually occur in an execution, so perturbing the schedule
   per seed strictly increases detection without adding flakiness: a race
   surfaced under yield fuzz is a real race. */
#define TSAN_FUZZ_SEEDS 3

static enum tsan_result run_tsan_check(void)
{
    printf("\nRace Detection (ThreadSanitizer):\n");

    const char *tsan_bin = "./test-sfs-tsan";
    const char *tsan_log = "tsan_output.log";

    char compile_cmd[512];
    snprintf(compile_cmd, sizeof compile_cmd,
             "gcc -std=c11 -g -fsanitize=thread -pthread -D_GNU_SOURCE=1 "
             "-I. -o %s test-sfs.c sfs-disk.c sfs-support.c 2>&1",
             tsan_bin);

    int rc = system(compile_cmd);
    if (rc != 0)
    {
        printf("  (skipped -- TSan compilation failed, gcc may not support "
               "-fsanitize=thread)\n");
        printf("  Note: without TSan, the C score reflects functional checks "
               "only; races are\n  unverified.  Rerun on a TSan-capable "
               "machine before trusting it.\n");
        unlink(tsan_bin);
        return TSAN_UNAVAILABLE; // don't penalize if TSan unavailable
    }

    /* On newer kernels (including WSL2), the TSan runtime can abort at
       startup with "FATAL: ThreadSanitizer: unexpected memory mapping"
       because of high-entropy ASLR.  That is an environment problem, not
       a student bug: retry under "setarch -R" (ASLR off for the child),
       and only report TSan unavailable if even that cannot start. */
    char wrap[96] = "";
    enum tsan_result result = TSAN_CLEAN;

    for (unsigned int seed = 1; seed <= TSAN_FUZZ_SEEDS; seed++)
    {
        char run_cmd[640];
        snprintf(run_cmd, sizeof run_cmd,
                 "timeout 20s %s%s --tsan-only --sched-fuzz=%u > %s 2>&1",
                 wrap, tsan_bin, seed, tsan_log);
        rc = system(run_cmd);

        if (log_contains(tsan_log, "FATAL: ThreadSanitizer"))
        {
            struct utsname un;
            if (wrap[0] == '\0' &&
                system("command -v setarch >/dev/null 2>&1") == 0 &&
                uname(&un) == 0)
            {
                snprintf(wrap, sizeof wrap, "setarch %s -R ", un.machine);
                seed--; /* retry the same seed with ASLR disabled */
                continue;
            }
            printf("  (skipped -- the TSan runtime cannot start in this "
                   "environment;\n"
                   "   kernel ASLR is incompatible with this TSan build. "
                   "Try 'sudo sysctl vm.mmap_rnd_bits=28'.)\n");
            printf("  Note: without TSan, the C score reflects functional "
                   "checks only; races are\n  unverified.  Rerun on a "
                   "TSan-capable machine before trusting it.\n");
            result = TSAN_UNAVAILABLE;
            break;
        }

        if (log_contains(tsan_log, "WARNING: ThreadSanitizer: data race"))
        {
            printf("  DATA RACE DETECTED (schedule-fuzz seed %u) -- "
                   "Category C score set to 0\n", seed);
            printf("  Race stacks were suppressed; see the lab writeup for "
                   "how to rerun TSan and print them.\n");
            result = TSAN_RACE;
            break;
        }
        if (WIFEXITED(rc) && WEXITSTATUS(rc) == 124)
        {
            printf("  TSan run timed out (schedule-fuzz seed %u) -- "
                   "Category C score set to 0\n", seed);
            result = TSAN_TIMEOUT;
            break;
        }
        if ((WIFEXITED(rc) && WEXITSTATUS(rc) != 0) || WIFSIGNALED(rc))
        {
            printf("  Sanitized C traces failed (schedule-fuzz seed %u) -- "
                   "Category C score set to 0\n", seed);
            result = TSAN_TRACE_FAILED;
            break;
        }
    }

    unlink(tsan_bin);
    unlink(tsan_log);
    cleanup_concurrency_disks();

    if (result == TSAN_CLEAN)
        printf("  No races detected across %d fuzzed schedules  PASS\n",
               TSAN_FUZZ_SEEDS);
    return result;
}

/* ================================================================== */
/*  Main -- run all traces and print scoreboard                         */
/* ================================================================== */

typedef int (*trace_fn)(void);

struct trace_entry
{
    const char *id;
    const char *name;
    trace_fn fn;
};

#define TRACE_TIMEOUT_SEC 30

/* Append 'n' bytes from 'src' to a dynamically-grown buffer whose state is
   held in the three out-params pbuf, pcap, plen.  Silently drops the data
   on allocation failure. */
static void capture_append(char **pbuf, size_t *pcap, size_t *plen,
                           const char *src, size_t n)
{
    if (*plen + n > *pcap)
    {
        size_t nc = *pcap ? *pcap : 4096;
        while (nc < *plen + n) nc *= 2;
        char *nb = realloc(*pbuf, nc);
        if (!nb) return;
        *pbuf = nb;
        *pcap = nc;
    }
    memcpy(*pbuf + *plen, src, n);
    *plen += n;
}

/* Run `fn` in a forked child with a TRACE_TIMEOUT_SEC wall-clock limit.
   Child exit code:
     0 = trace passed
     1 = trace failed (a CHECK inside it failed and set trace_ok = 0)

   The child's stderr is redirected into a pipe so the parent can append
   the captured FAIL lines to *out_buf (grown with capture_append). This
   lets run_category print the summary line BEFORE the FAIL details, same
   as the memstream path for A/B traces. TIMEOUT/CRASH diagnostics are
   appended to the capture too, so they share the same ordering rules.
   Returns 1 if the trace passed, 0 otherwise. */
static int run_trace_with_timeout(const char *id, trace_fn fn,
                                  char **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;
    size_t cap = 0;

    fflush(stdout);
    fflush(stderr);

    int pfd[2];
    if (pipe(pfd) < 0)
    {
        fprintf(stderr, "pipe() failed: %s\n", strerror(errno));
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        close(pfd[0]); close(pfd[1]);
        fprintf(stderr, "fork() failed: %s\n", strerror(errno));
        return 0;
    }
    if (pid == 0)
    {
        close(pfd[0]);
        if (dup2(pfd[1], STDERR_FILENO) < 0) _exit(2);
        close(pfd[1]);
        /* Line-buffer stderr so CHECK's single fputs flushes out immediately
           and pipe writes stay atomic (each <= PIPE_BUF). */
        setvbuf(stderr, NULL, _IOLBF, 0);
        int ok = fn();
        trace_fail_tail(stderr);
        fflush(stdout);
        fflush(stderr);
        _exit(ok ? 0 : 1);
    }

    /* Parent: drain pipe while polling for child exit. */
    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL);
    if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    int elapsed_ms = 0;
    const int step_ms = 100;
    int status = 0;
    int timed_out = 0;

    for (;;)
    {
        /* Drain whatever data is currently in the pipe. */
        char tmp[4096];
        for (;;)
        {
            ssize_t n = read(pfd[0], tmp, sizeof tmp);
            if (n > 0)
                capture_append(out_buf, &cap, out_len, tmp, (size_t)n);
            else if (n == 0)
                break;  /* EOF: child closed write end */
            else if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
                break;
        }

        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0)
        {
            fprintf(stderr, "waitpid: %s\n", strerror(errno));
            close(pfd[0]);
            return 0;
        }
        if (elapsed_ms >= TRACE_TIMEOUT_SEC * 1000)
        {
            timed_out = 1;
            break;
        }

        /* Sleep up to step_ms, but wake early if pipe becomes readable. */
        struct pollfd pf = { .fd = pfd[0], .events = POLLIN };
        poll(&pf, 1, step_ms);
        elapsed_ms += step_ms;
    }

    if (timed_out)
    {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        char *hint = keep_failure_images(id);
        cleanup_concurrency_disks();
        /* Drain anything the child managed to emit before the kill. */
        char tmp2[4096];
        for (;;)
        {
            ssize_t n = read(pfd[0], tmp2, sizeof tmp2);
            if (n <= 0) break;
            capture_append(out_buf, &cap, out_len, tmp2, (size_t)n);
        }
        char msg[160];
        int m = snprintf(msg, sizeof msg,
                         "       -> TIMEOUT: trace %s exceeded %d seconds "
                         "(deadlock suspected); killed\n",
                         id, TRACE_TIMEOUT_SEC);
        if (m > 0) capture_append(out_buf, &cap, out_len, msg, (size_t)m);
        if (hint)
        {
            capture_append(out_buf, &cap, out_len, hint, strlen(hint));
            free(hint);
        }
        close(pfd[0]);
        return 0;
    }

    /* Final drain after child exit (EOF expected). */
    char tmp[4096];
    for (;;)
    {
        ssize_t n = read(pfd[0], tmp, sizeof tmp);
        if (n <= 0) break;
        capture_append(out_buf, &cap, out_len, tmp, (size_t)n);
    }
    close(pfd[0]);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        cleanup_concurrency_disks();
        return 1;
    }

    /* Failure: snapshot the trace's image(s) before cleanup unlinks them. */
    char *hint = keep_failure_images(id);
    cleanup_concurrency_disks();
    if (WIFSIGNALED(status))
    {
        char msg[96];
        int m = snprintf(msg, sizeof msg,
                         "       -> CRASH: trace %s killed by signal %d\n",
                         id, WTERMSIG(status));
        if (m > 0) capture_append(out_buf, &cap, out_len, msg, (size_t)m);
    }
    if (hint)
    {
        capture_append(out_buf, &cap, out_len, hint, strlen(hint));
        free(hint);
    }
    return 0;
}

static int run_category(const char *label, struct trace_entry *traces, int n)
{
    printf("\nCategory %s:\n", label);
    int passed = 0;
    for (int i = 0; i < n; i++)
    {
        char *buf = NULL;
        size_t buflen = 0;

        atomic_store_explicit(&trace_fail_count, 0, memory_order_relaxed);
        /* Every trace runs in a forked child with a timeout, so a deadlock,
           crash, or assertion failure in the student's implementation fails
           that one trace instead of taking down the autograder.  The child's
           stderr is captured via a pipe; the returned buf holds FAIL details
           (and TIMEOUT/CRASH if any) for the parent to print after the
           summary line. */
        int ok = run_trace_with_timeout(traces[i].id, traces[i].fn,
                                        &buf, &buflen);
        passed += ok;
        printf("  %s %-24s %s  [%d/1]\n", traces[i].id, traces[i].name,
               ok ? "PASS" : "FAIL", ok);
        fflush(stdout);
        if (buf && buflen > 0) fwrite(buf, 1, buflen, stderr);
        fflush(stderr);
        free(buf);
    }
    printf("  Subtotal: %d/%d\n", passed, n);
    return passed;
}

int main(int argc, char *argv[])
{
    init_disk_paths();

    int mode_tsan_only = 0;
    int mode_perf_only = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0)
            quiet_mode = 1;
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
            trace_fail_limit = INT_MAX;
        else if (strcmp(argv[i], "--tsan-only") == 0)
            mode_tsan_only = 1;
        else if (strcmp(argv[i], "--perf-only") == 0)
            mode_perf_only = 1;
        else if (strcmp(argv[i], "--sched-fuzz") == 0)
            sched_fuzz_seed = 1;
        else if (strncmp(argv[i], "--sched-fuzz=", 13) == 0)
            sched_fuzz_seed = (unsigned int)strtoul(argv[i] + 13, NULL, 10);
        else
            fprintf(stderr, "warning: unknown argument '%s' (ignored)\n", argv[i]);
    }

    if (sched_fuzz_seed == 0)
    {
        const char *sf = getenv("SFS_SCHED_FUZZ_SEED");
        if (sf && *sf)
            sched_fuzz_seed = (unsigned int)strtoul(sf, NULL, 10);
    }
    if (sched_fuzz_seed != 0)
        fprintf(stderr, "Schedule fuzz active (seed %u)\n", sched_fuzz_seed);

    keep_failed_disks = !mode_tsan_only;
    const char *kfd = getenv("SFS_KEEP_FAILED_DISKS");
    if (kfd && *kfd)
        keep_failed_disks = strcmp(kfd, "0") != 0;

    /* --tsan-only: run only the C traces. TSan reports races via its own
       runtime; exit code 66 signals the C traces themselves failed. */
    if (mode_tsan_only)
    {
        struct trace_entry cat_c[] = {
            {"C00", "separate_files", trace_C00},
            {"C01", "read_same_file", trace_C01},
            {"C02", "rw_mix_storm_churn", trace_C02},
        };
        quiet_mode = 1;
        serialize_c_api_calls = 0;
        int c = run_category("C (TSan)", cat_c, 3);
        unlink(DISK_NAME);
        cleanup_concurrency_disks();
        return (c == 3) ? 0 : 66;
    }

    /* --perf-only: used by test-sfs-baseline to print baseline ops/sec.
       Writes a single decimal number (no scoring, no banners) to stdout. */
    if (mode_perf_only)
    {
        double baseline_ops = run_perf_benchmark_raw();
        printf("%.2f\n", baseline_ops);
        if (atomic_load_explicit(&perf_worker_failed, memory_order_relaxed))
            fprintf(stderr, "warning: perf workload self-checks failed; "
                    "this number is not meaningful\n");
        unlink(DISK_NAME);
        return 0;
    }

    struct trace_entry cat_a[] = {
        {"A00", "format_mount", trace_A00},
        {"A01", "open_close_rw", trace_A01},
        {"A02", "getpos", trace_A02},
        {"A03", "seek", trace_A03},
        {"A04", "rename", trace_A04},
    };

    struct trace_entry cat_b[] = {
        {"B00", "remove_list", trace_B00},
        {"B01", "multi_block_seek", trace_B01},
        {"B02", "edge_cases", trace_B02},
        {"B03", "rename_list_edges", trace_B03},
    };

    struct trace_entry cat_c[] = {
        {"C00", "separate_files", trace_C00},
        {"C01", "read_same_file", trace_C01},
        {"C02", "rw_mix_storm_churn", trace_C02},
    };

    printf("========================================\n");
    printf("        SFS Lab Autograder\n");
    printf("========================================\n");

    int a = run_category("A (Feature Tests)", cat_a, 5);
    int b = run_category("B (Sequential Correctness)", cat_b, 4);
    int c = run_category("C (Concurrent Correctness)", cat_c, 3);

    /* TSan race detection: if races found, C score becomes 0.  Run it only
       after the normal correctness suite passes, so partially implemented
       starters get stable trace feedback before sanitizer diagnostics. */
    enum tsan_result tsan = TSAN_SKIPPED;
    if (a == 5 && b == 4 && c == 3)
        tsan = run_tsan_check();
    else
    {
        printf("\nRace Detection (ThreadSanitizer):\n");
        printf("  (skipped -- normal correctness traces must all pass first)\n");
    }
    if (tsan == TSAN_RACE || tsan == TSAN_TRACE_FAILED ||
        tsan == TSAN_TIMEOUT)
        c = 0;

    int correctness = a + b + c;
    printf("\nCorrectness: %d/12\n", correctness);

    printf("\nPerformance:\n");
    int perf = 0;
    if (correctness < 12)
    {
        printf("  (skipped -- correctness tests must all pass first)\n");
        printf("  Score: 0/10\n");
    }
    else
    {
        /* Perf benchmark also needs deadlock protection -- 60s budget
           (longer than a Category C trace because slower machines may
           legitimately take more time on the full workload). */
        fflush(stdout); fflush(stderr);
        pid_t pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "fork() for perf failed: %s\n", strerror(errno));
            perf = 0;
        }
        else if (pid == 0)
        {
            int score = run_perf_benchmark();
            /* Flush stdio so the child's "Student throughput", "Baseline",
               and "Ratio" lines survive output redirection. */
            fflush(stdout);
            fflush(stderr);
            _exit(score);
        }
        else
        {
            int status = 0;
            int elapsed_ms = 0;
            const int step_ms = 100;
            const int perf_budget_ms = 60 * 1000;
            while (elapsed_ms < perf_budget_ms)
            {
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid) break;
                if (r < 0)
                {
                    fprintf(stderr, "waitpid(perf): %s\n", strerror(errno));
                    break;
                }
                struct timespec ts = {
                    .tv_sec = 0, .tv_nsec = step_ms * 1000000L
                };
                nanosleep(&ts, NULL);
                elapsed_ms += step_ms;
            }
            if (elapsed_ms >= perf_budget_ms)
            {
                fprintf(stderr,
                        "  TIMEOUT: perf benchmark exceeded %ds; killing\n",
                        perf_budget_ms / 1000);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                unlink(PERF_DISK);
                perf = 0;
            }
            else if (WIFEXITED(status))
            {
                perf = WEXITSTATUS(status);
            }
            else
            {
                perf = 0;
            }
        }
        printf("  Score: %d/10\n", perf);
    }

    int total = correctness + perf;
    printf("\n----------------------------------------\n");
    printf("  Total: %d/22  (+ up to 4 style pts)\n", total);
    printf("========================================\n");

    unlink(DISK_NAME);
    /* Deliberate contract: the exit status is the *correctness* gate
       (12/12 -> 0), so scripts can distinguish "implementation complete"
       from "still failing traces".  The performance score never changes
       the exit status; read it from the scoreboard. */
    return (correctness == 12) ? 0 : 1;
}
