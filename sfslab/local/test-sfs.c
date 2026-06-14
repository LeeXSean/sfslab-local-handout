/*
 * test-sfs.c - SFS Lab Autograder (local, no Lua required)
 *
 * Mirrors the original CMU grading structure:
 *   Category A (5 pts) - Feature tests
 *   Category B (4 pts) - Sequential correctness
 *   Category C (3 pts) - Concurrent correctness
 *   Performance  (10 pts) - Concurrent throughput benchmark
 *   Style        (4 pts) - Manual review (not auto-graded)
 *
 * Build:  make test-sfs
 * Run:    ./test-sfs
 */

#include "sfs-api.h"
#include "test-report.h"

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

/* Disk image paths. Default to the current directory; override with
   SFS_DISK_DIR=/some/path (e.g. SFS_DISK_DIR=/tmp on Docker Desktop
   to keep perf I/O off the bind mount -- see writeup Section 5.4). */
static const char *DISK_NAME = "test.img";
static const char *CONC_DISK_C00 = "test_conc_C00.img";
static const char *CONC_DISK_C01 = "test_conc_C01.img";
static const char *CONC_DISK_C02_RW = "test_conc_C02_rw.img";
static const char *CONC_DISK_C02_STORM = "test_conc_C02_storm.img";
static const char *CONC_DISK_C02_DIR = "test_conc_C02_dir.img";
static const char *CONC_DISK_S01 = "test_conc_S01.img";
static const char *MODEL_DISK = "test_model.img";
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
    CONC_DISK_S01 = sfs_join(dir, "test_conc_S01.img");
    MODEL_DISK = sfs_join(dir, "test_model.img");
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
    unlink(CONC_DISK_S01);
    unlink(MODEL_DISK);
}

/* Keep a copy of a failing trace's disk image(s) for post-mortem fsck.
   On by default for human runs; off under --json and --tsan-only so
   report generation stays clean.  SFS_KEEP_FAILED_DISKS=0/1 overrides.
   The copies match the *.img cleanup globs (make clean, .gitignore). */
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
    /* X traces are optional: in the quiet default run a not-achieved X
       trace is the normal state, not a failure worth keeping artifacts
       for.  --x-only runs them with detail and does keep images. */
    if (id[0] == 'X' && quiet_mode)
        return NULL;

    /* A/B/X traces share DISK_NAME; C and stress traces use their own
       images.  Restrict candidates so a leftover test.img from an earlier
       category is not misattributed to a concurrent trace. */
    const char *cands[6];
    size_t n_cands = 0;
    if (id[0] == 'C' || id[0] == 'S')
    {
        cands[n_cands++] = CONC_DISK_C00;
        cands[n_cands++] = CONC_DISK_C01;
        cands[n_cands++] = CONC_DISK_C02_RW;
        cands[n_cands++] = CONC_DISK_C02_STORM;
        cands[n_cands++] = CONC_DISK_C02_DIR;
        cands[n_cands++] = CONC_DISK_S01;
    }
    else if (id[0] == 'M')
    {
        cands[n_cands++] = MODEL_DISK;
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
static FILE *check_stream;             /* where CHECK writes FAIL diagnostics */

/* Build the full "       -> FAIL ... \n" line into a local buffer, then emit
   with a single fputs so concurrent threads writing to the same FILE (e.g.
   the pipe back to the parent in forked C traces) can't interleave each
   other's lines.  Pipe writes of <= PIPE_BUF bytes are atomic on Linux;
   512 bytes is well within that. */
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
                FILE *_cs = check_stream ? check_stream : stderr;              \
                fputs(_lb, _cs);                                               \
                fflush(_cs);                                                   \
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

static _Atomic int dir_churn_start;
static _Atomic int dir_churn_done;

static void wait_dir_churn_start(void)
{
    while (!atomic_load_explicit(&dir_churn_start, memory_order_acquire))
        sched_yield();
}

static void *thread_dir_churn(void *arg)
{
    int id = *(int *)arg;
    wait_dir_churn_start();

    for (int i = 0; i < 20; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        int n = snprintf(name, sizeof name, "dir%d_%02d", id, i);
        if (n < 0 || (size_t)n >= sizeof name)
            return (void *)(intptr_t)-1;

        fuzz_point();
        int fd = sfs_open(name);
        if (fd < 0)
            return (void *)(intptr_t)-1;

        char data[32];
        int len = snprintf(data, sizeof data, "dir-%d-%d", id, i);
        fuzz_point();
        if (len < 0 || sfs_write(fd, data, (size_t)len) != (ssize_t)len)
        {
            sfs_close(fd);
            return (void *)(intptr_t)-1;
        }
        sfs_close(fd);

        fuzz_point();
        if (sfs_remove(name) != 0)
            return (void *)(intptr_t)-1;
        sched_yield();
    }
    return NULL;
}

static void *thread_list_during_churn(void *arg)
{
    wait_dir_churn_start();

    int passes = 0;
    do
    {
        sfs_list_cookie cookie = NULL;
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        int status;
        fuzz_point();
        while ((status = sfs_list(&cookie, name, sizeof name)) == 0)
        {
            if (name[0] == '\0')
                return (void *)(intptr_t)-1;
        }
        if (status != 1)
            return (void *)(intptr_t)-1;
        passes++;
        sched_yield();
    } while (!atomic_load_explicit(&dir_churn_done, memory_order_acquire) ||
             passes < 4);

    return NULL;
}

/* C02: mixed r/w + open/close stress */
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
    return trace_ok;
}

/* Stress-only: directory churn while another thread lists.
   This is intentionally diagnostic rather than part of the 22-point score:
   the workload is useful for shaking out unstable locking, but it is more
   schedule-sensitive than the official-style C correctness signal. */
static int trace_stress_dir_churn(void)
{
    trace_ok = 1;
    sfs_format(CONC_DISK_C02_DIR, disk_size());
    atomic_store_explicit(&dir_churn_start, 0, memory_order_release);
    atomic_store_explicit(&dir_churn_done, 0, memory_order_release);

    pthread_t lister;
    pthread_t dir_threads[NUM_THREADS];
    int dir_ids[NUM_THREADS];
    pthread_create(&lister, NULL, thread_list_during_churn, NULL);
    for (int i = 0; i < NUM_THREADS; i++)
    {
        dir_ids[i] = i;
        pthread_create(&dir_threads[i], NULL, thread_dir_churn, &dir_ids[i]);
    }
    atomic_store_explicit(&dir_churn_start, 1, memory_order_release);

    int ok = 1;
    for (int i = 0; i < NUM_THREADS; i++)
    {
        void *ret;
        pthread_join(dir_threads[i], &ret);
        if (ret != NULL)
            ok = 0;
    }
    atomic_store_explicit(&dir_churn_done, 1, memory_order_release);

    void *list_ret;
    pthread_join(lister, &list_ret);
    if (list_ret != NULL)
        ok = 0;
    CHECK(ok, "concurrent directory churn failed");

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    int status;
    while ((status = sfs_list(&cookie, name, sizeof name)) == 0)
        count++;
    CHECK(status == 1, "final list returned %d", status);
    CHECK(count == 0, "directory churn: expected 0 files, got %d", count);

    unmount_and_check(CONC_DISK_C02_DIR);
    unlink(CONC_DISK_C02_DIR);
    return trace_ok;
}

/* Stress-only: rename atomicity.
   sfs-api.h promises that overwrite-rename leaves no window in which a
   concurrent thread can observe new_name not existing.  sfs_open creates
   missing files as empty, which makes it a natural gap detector: 'stable'
   always carries payload bytes, so an observer that reads 0 bytes from it
   can only have open()-created it inside a rename gap. */
#define RENAME_ATOMIC_ITERS 150
#define RENAME_OBSERVERS 3

static _Atomic int rename_atomic_stop;
static _Atomic int rename_atomic_gaps;
static _Atomic int rename_atomic_writer_err;
static _Atomic int rename_atomic_observer_err;

static void *thread_rename_writer(void *arg)
{
    for (int i = 0; i < RENAME_ATOMIC_ITERS; i++)
    {
        fuzz_point();
        int fd = sfs_open("incoming");
        if (fd < 0)
        {
            atomic_store(&rename_atomic_writer_err, fd);
            break;
        }
        char data[32];
        int len = snprintf(data, sizeof data, "payload-%03d", i);
        fuzz_point();
        if (len < 0 || sfs_write(fd, data, (size_t)len) != (ssize_t)len)
        {
            sfs_close(fd);
            atomic_store(&rename_atomic_writer_err, -EIO);
            break;
        }
        sfs_close(fd);

        /* -EBUSY only means an observer momentarily holds 'stable' open
           (replacing an open file is the X02 optional challenge); retry. */
        int r;
        do
        {
            fuzz_point();
            r = sfs_rename("incoming", "stable");
            if (r == -EBUSY)
                sched_yield();
        } while (r == -EBUSY);
        if (r != 0)
        {
            atomic_store(&rename_atomic_writer_err, r);
            break;
        }
    }
    atomic_store_explicit(&rename_atomic_stop, 1, memory_order_release);
    return NULL;
}

static void *thread_rename_observer(void *arg)
{
    while (!atomic_load_explicit(&rename_atomic_stop, memory_order_acquire))
    {
        fuzz_point();
        int fd = sfs_open("stable");
        if (fd < 0)
        {
            atomic_store(&rename_atomic_observer_err, fd);
            return NULL;
        }
        char buf[64];
        fuzz_point();
        ssize_t nr = sfs_read(fd, buf, sizeof buf);
        sfs_close(fd);
        if (nr == 0)
            atomic_fetch_add(&rename_atomic_gaps, 1);
        else if (nr < 0)
        {
            atomic_store(&rename_atomic_observer_err, (int)nr);
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

static int trace_stress_rename_atomic(void)
{
    trace_ok = 1;
    sfs_format(CONC_DISK_S01, disk_size());
    atomic_store(&rename_atomic_stop, 0);
    atomic_store(&rename_atomic_gaps, 0);
    atomic_store(&rename_atomic_writer_err, 0);
    atomic_store(&rename_atomic_observer_err, 0);

    /* Seed 'stable' with payload before any observer looks at it. */
    int fd = sfs_open("stable");
    CHECK(fd >= 0, "initial open of 'stable' returned %d", fd);
    CHECK(sfs_write(fd, "payload-init", 12) == 12, "seeding 'stable' failed");
    sfs_close(fd);

    pthread_t writer;
    pthread_t observers[RENAME_OBSERVERS];
    pthread_create(&writer, NULL, thread_rename_writer, NULL);
    for (int i = 0; i < RENAME_OBSERVERS; i++)
        pthread_create(&observers[i], NULL, thread_rename_observer, NULL);

    pthread_join(writer, NULL);
    for (int i = 0; i < RENAME_OBSERVERS; i++)
        pthread_join(observers[i], NULL);

    int werr = atomic_load(&rename_atomic_writer_err);
    int oerr = atomic_load(&rename_atomic_observer_err);
    int gaps = atomic_load(&rename_atomic_gaps);
    CHECK(werr == 0, "rename writer failed with %d", werr);
    CHECK(oerr == 0, "observer hit unexpected error %d", oerr);
    CHECK(gaps == 0,
          "observers saw 'stable' missing %d time(s): overwrite rename must "
          "be atomic (no window where new_name does not exist)", gaps);

    /* Final state: the last rename left exactly one file, 'stable',
       holding the writer's last payload. */
    fd = sfs_open("stable");
    CHECK(fd >= 0, "final open of 'stable' returned %d", fd);
    char buf[64] = {0};
    ssize_t nr = sfs_read(fd, buf, sizeof buf);
    char want[32];
    int wantlen = snprintf(want, sizeof want, "payload-%03d",
                           RENAME_ATOMIC_ITERS - 1);
    CHECK(werr != 0 || (nr == wantlen && memcmp(buf, want, (size_t)wantlen) == 0),
          "final 'stable' content mismatch: got %zd bytes '%.*s'",
          nr, (int)(nr > 0 ? nr : 0), buf);
    sfs_close(fd);

    sfs_list_cookie cookie = NULL;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    while (sfs_list(&cookie, name, sizeof name) == 0)
        count++;
    CHECK(count == 1, "expected only 'stable' to remain, found %d files",
          count);

    unmount_and_check(CONC_DISK_S01);
    unlink(CONC_DISK_S01);
    return trace_ok;
}

/* ================================================================== */
/*  Category X -- Optional Challenges (not scored)                     */
/* ================================================================== */

/* These make the three "optional challenge" comments in sfs-disk.c (and
   the X traces that sfs-disk.h alludes to) executable.  They are
   exploration targets, not graded work: the scoreboard reports them as
   achieved or not, and nothing here changes the 22-point score.  The
   unmodified starter is expected to achieve none of them. */

/* X00: empty files should consume no data blocks (see the createFile
   comment in sfs-disk.c).  On a one-page disk with N data blocks,
   create a few empty files and then grow one file to exactly N blocks;
   that only fits if the empty files allocated nothing.  The handout
   starter fails because every file occupies at least one block.

   This trace skips sfs-fsck on purpose: implementing X00 changes the
   "directory entry in use" rule, which requires a matching change in
   sfs-fsck.c's check_directory_entries (see the comment there). */
static int trace_X00(void)
{
    trace_ok = 1;
    size_t disk_bytes = (size_t)sysconf(_SC_PAGESIZE);
    size_t data_blocks = disk_bytes / 512u - 1; /* minus the superblock */

    int r = sfs_format(DISK_NAME, disk_bytes);
    CHECK(r == 0, "sfs_format(%zu) returned %d", disk_bytes, r);
    if (r != 0)
        return trace_ok;

    for (int i = 0; i < 5; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        snprintf(name, sizeof name, "empty%d", i);
        int fd = sfs_open(name);
        CHECK(fd >= 0, "open(\"%s\") returned %d", name, fd);
        if (fd < 0)
            goto out;
        sfs_close(fd);
    }

    int fd = sfs_open("big");
    CHECK(fd >= 0, "open(\"big\") returned %d", fd);
    if (fd < 0)
        goto out;
    char chunk[500]; /* BLOCK_DATA_SIZE in sfs-disk.h */
    memset(chunk, 'X', sizeof chunk);
    for (size_t b = 0; b < data_blocks; b++)
    {
        ssize_t nw = sfs_write(fd, chunk, sizeof chunk);
        CHECK(nw == (ssize_t)sizeof chunk,
              "growing \"big\" to block %zu of %zu returned %zd "
              "(zero-block empty files would have left room)",
              b + 1, data_blocks, nw);
        if (nw != (ssize_t)sizeof chunk)
        {
            sfs_close(fd);
            goto out;
        }
    }
    sfs_close(fd);

    sfs_list_cookie cookie = NULL;
    char lname[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    while (sfs_list(&cookie, lname, sizeof lname) == 0)
        count++;
    CHECK(count == 6, "expected 6 files (5 empty + big), got %d", count);

    fd = sfs_open("big");
    char buf[8] = {0};
    CHECK(fd >= 0 && sfs_read(fd, buf, 4) == 4 && memcmp(buf, "XXXX", 4) == 0,
          "read-back of \"big\" failed");
    sfs_close(fd);

    for (int i = 0; i < 5; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        snprintf(name, sizeof name, "empty%d", i);
        CHECK(sfs_remove(name) == 0, "remove(\"%s\") failed", name);
    }
    CHECK(sfs_remove("big") == 0, "remove(\"big\") failed");

out:
    sfs_unmount(); /* no fsck here: see the header comment */
    return trace_ok;
}

/* X01: the root directory should be able to grow past the superblock's
   15 entries (see the sfs_open comment in sfs-disk.c).  sfs-fsck
   already validates extended root-directory chains, so this trace keeps
   the usual post-unmount consistency check.  The starter fails at file
   16 with -ENOSPC. */
#define X01_FILES 20
static int trace_X01(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    for (int i = 0; i < X01_FILES; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        snprintf(name, sizeof name, "ext%02d", i);
        int fd = sfs_open(name);
        CHECK(fd >= 0,
              "open(\"%s\") returned %d (file %d of %d requires directory "
              "expansion)", name, fd, i + 1, X01_FILES);
        if (fd < 0)
            goto out;
        char c = (char)('a' + i % 26);
        CHECK(sfs_write(fd, &c, 1) == 1, "write to \"%s\" failed", name);
        sfs_close(fd);
    }

    char seen[X01_FILES] = {0};
    sfs_list_cookie cookie = NULL;
    char lname[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    int status;
    while ((status = sfs_list(&cookie, lname, sizeof lname)) == 0)
    {
        count++;
        int idx = -1;
        if (strncmp(lname, "ext", 3) == 0)
            idx = atoi(lname + 3);
        CHECK(idx >= 0 && idx < X01_FILES && !seen[idx],
              "list returned unexpected or duplicate name \"%s\"", lname);
        if (idx >= 0 && idx < X01_FILES)
            seen[idx] = 1;
    }
    CHECK(status == 1, "list should end with +1, got %d", status);
    CHECK(count == X01_FILES, "expected %d files in the listing, got %d",
          X01_FILES, count);

    int fd = sfs_open("ext07");
    char buf[4] = {0};
    CHECK(fd >= 0 && sfs_read(fd, buf, 1) == 1 && buf[0] == 'h',
          "read-back of \"ext07\" failed");
    sfs_close(fd);

    for (int i = 0; i < X01_FILES; i++)
    {
        char name[SFS_FILE_NAME_SIZE_LIMIT];
        snprintf(name, sizeof name, "ext%02d", i);
        CHECK(sfs_remove(name) == 0, "remove(\"%s\") failed", name);
    }

out:
    unmount_and_check(DISK_NAME);
    return trace_ok;
}

/* X02: Unix delete-while-open semantics (see the sfs_remove comment in
   sfs-disk.c): removing an open file succeeds, the name disappears
   immediately, and the data stays readable through the open fd until
   close, at which point the storage is reclaimed.

   Heads up: achieving X02 deliberately conflicts with the graded B02
   expectation that remove(open file) == -EBUSY.  It is a design
   exploration, not extra credit; the writeup discusses the trade-off.
   The starter returns -EBUSY here, which reports X02 as not achieved. */
static int trace_X02(void)
{
    trace_ok = 1;
    sfs_format(DISK_NAME, disk_size());

    int fd = sfs_open("victim");
    CHECK(fd >= 0, "open(\"victim\") returned %d", fd);
    CHECK(sfs_write(fd, "DATA", 4) == 4, "write to \"victim\" failed");

    int r = sfs_remove("victim");
    CHECK(r == 0,
          "remove of an open file should succeed with Unix semantics, "
          "got %d", r);
    if (r != 0)
    {
        sfs_close(fd);
        goto out;
    }

    sfs_list_cookie cookie = NULL;
    char lname[SFS_FILE_NAME_SIZE_LIMIT];
    int count = 0;
    while (sfs_list(&cookie, lname, sizeof lname) == 0)
        count++;
    CHECK(count == 0, "\"victim\" should leave the directory immediately, "
          "but %d file(s) are listed", count);

    ssize_t p = sfs_seek(fd, -4);
    CHECK(p == 0, "seek(-4) on the unlinked fd returned %zd", p);
    char buf[8] = {0};
    CHECK(sfs_read(fd, buf, 4) == 4 && memcmp(buf, "DATA", 4) == 0,
          "unlinked fd should still read its data");
    CHECK(sfs_write(fd, "MORE", 4) == 4,
          "unlinked fd should still accept writes");

    int fd2 = sfs_open("victim");
    CHECK(fd2 >= 0 && fd2 != fd,
          "re-opening \"victim\" should create a fresh file");
    if (fd2 >= 0)
    {
        CHECK(sfs_read(fd2, buf, 4) == 0,
              "the fresh \"victim\" should be empty");
        sfs_close(fd2);
        CHECK(sfs_remove("victim") == 0, "remove of fresh \"victim\" failed");
    }
    sfs_close(fd); /* storage for the unlinked file is reclaimed here */

out:
    unmount_and_check(DISK_NAME); /* fsck must find no orphaned blocks */
    return trace_ok;
}

/* ================================================================== */
/*  Differential model fuzz (--model-fuzz, diagnostic)                 */
/* ================================================================== */

/* A seeded random op sequence is applied in lockstep to the student
   implementation and to a small in-memory reference model; any
   disagreement in return values, file data, or directory contents is
   reported together with the seed and the most recent operations, so
   `./test-sfs --model-fuzz=SEED` replays the failure exactly.

   The model encodes the graded handout contract (including error codes
   the B traces already rely on: -EBADF, -EBUSY, -ENOENT, -ENOSPC,
   -EMFILE, -ENAMETOOLONG).  -ENOSYS from the three starter stubs makes
   the op a no-op on both sides, so the fuzz is useful from the first
   build onward.  Ops with deliberately unspecified semantics (renaming
   a file that is open, rename onto itself) are never generated.

   The disk is one page -- the smallest image sfs_format accepts -- so
   ENOSPC paths get exercised for real on common 4K-page systems. */

#define MF_MAX_FILES 15   /* = DIR_ENTRIES_PER_BLOCK in sfs-disk.h */
#define MF_MAX_FDS 32     /* = OPEN_FILE_LIMIT in sfs-disk.c */
#define MF_BLOCK_DATA 500u /* = BLOCK_DATA_SIZE in sfs-disk.h */
#define MF_FILE_CAP 3500u /* 7 data blocks: a full 4K-page disk */
#define MF_NAMES 12
#define MF_DEFAULT_OPS 2000
#define MF_CHECKPOINT_EVERY 250
#define MF_LOG_LINES 30

static unsigned int model_fuzz_seed = 1;

struct mf_file
{
    int live;
    char name[SFS_FILE_NAME_SIZE_LIMIT];
    size_t size;
};

struct mf_fd
{
    int live;
    int file;
    size_t pos;
};

static struct mf_file mf_files[MF_MAX_FILES];
static char mf_data[MF_MAX_FILES][MF_FILE_CAP];
static struct mf_fd mf_fds[MF_MAX_FDS];
static size_t mf_total_blocks;
static uint64_t mf_rng;
static int mf_unimpl_calls;

static char mf_log[MF_LOG_LINES][112];
static int mf_log_next;
static int mf_log_count;

static uint64_t mf_rand(void)
{
    mf_rng ^= mf_rng >> 12;
    mf_rng ^= mf_rng << 25;
    mf_rng ^= mf_rng >> 27;
    return mf_rng * 0x2545F4914F6CDD1Dull;
}

static unsigned int mf_rand_below(unsigned int n)
{
    return (unsigned int)(mf_rand() % n);
}

static void mf_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mf_log[mf_log_next], sizeof mf_log[0], fmt, ap);
    va_end(ap);
    mf_log_next = (mf_log_next + 1) % MF_LOG_LINES;
    if (mf_log_count < MF_LOG_LINES)
        mf_log_count++;
}

static void mf_dump_log(void)
{
    if (quiet_mode)
        return;
    FILE *out = check_stream ? check_stream : stderr;
    fprintf(out, "       -> last %d op(s), oldest first:\n", mf_log_count);
    int start = (mf_log_next - mf_log_count + MF_LOG_LINES) % MF_LOG_LINES;
    for (int i = 0; i < mf_log_count; i++)
        fprintf(out, "          %s\n", mf_log[(start + i) % MF_LOG_LINES]);
    fflush(out);
}

static int mf_find(const char *name)
{
    for (int i = 0; i < MF_MAX_FILES; i++)
        if (mf_files[i].live && strcmp(mf_files[i].name, name) == 0)
            return i;
    return -1;
}

static size_t mf_blocks_of(size_t size)
{
    return size == 0 ? 1 : (size + MF_BLOCK_DATA - 1) / MF_BLOCK_DATA;
}

static size_t mf_used_blocks(void)
{
    size_t used = 0;
    for (int i = 0; i < MF_MAX_FILES; i++)
        if (mf_files[i].live)
            used += mf_blocks_of(mf_files[i].size);
    return used;
}

static int mf_open_fd_count(void)
{
    int n = 0;
    for (int i = 0; i < MF_MAX_FDS; i++)
        n += mf_fds[i].live;
    return n;
}

static int mf_file_has_open_fd(int file)
{
    for (int i = 0; i < MF_MAX_FDS; i++)
        if (mf_fds[i].live && mf_fds[i].file == file)
            return 1;
    return 0;
}

static int mf_create(const char *name)
{
    for (int i = 0; i < MF_MAX_FILES; i++)
    {
        if (!mf_files[i].live)
        {
            mf_files[i].live = 1;
            mf_files[i].size = 0;
            snprintf(mf_files[i].name, sizeof mf_files[i].name, "%s", name);
            memset(mf_data[i], 0, MF_FILE_CAP);
            return i;
        }
    }
    return -1;
}

/* Pick a name from the pool; 1 in 16 picks is an over-limit name so the
   -ENAMETOOLONG paths stay covered. */
static const char *mf_pick_name(char buf[], size_t cap)
{
    unsigned int sel = mf_rand_below(16);
    if (sel == 15 && cap > SFS_FILE_NAME_SIZE_LIMIT + 4)
    {
        memset(buf, 'x', SFS_FILE_NAME_SIZE_LIMIT + 3);
        buf[SFS_FILE_NAME_SIZE_LIMIT + 3] = '\0';
        return buf;
    }
    snprintf(buf, cap, "f%02u", sel % MF_NAMES);
    return buf;
}

/* Mostly a live fd; 1 in 8 picks is deliberately bogus (-1, one past
   the table, or a random slot that may be dead). */
static int mf_pick_fd(void)
{
    if (mf_rand_below(8) == 0)
    {
        unsigned int sel = mf_rand_below(3);
        if (sel == 0)
            return -1;
        if (sel == 1)
            return MF_MAX_FDS;
        return (int)mf_rand_below(MF_MAX_FDS);
    }
    int live[MF_MAX_FDS];
    int n = 0;
    for (int i = 0; i < MF_MAX_FDS; i++)
        if (mf_fds[i].live)
            live[n++] = i;
    if (n == 0)
        return -1;
    return live[mf_rand_below((unsigned int)n)];
}

static int mf_fd_valid(int fd)
{
    return fd >= 0 && fd < MF_MAX_FDS && mf_fds[fd].live;
}

#define MF_FAIL(...)                                                           \
    do                                                                         \
    {                                                                          \
        CHECK(0, __VA_ARGS__);                                                 \
        mf_dump_log();                                                         \
        goto mf_done;                                                          \
    } while (0)

static int trace_model_fuzz(void)
{
    trace_ok = 1;
    memset(mf_files, 0, sizeof mf_files);
    memset(mf_fds, 0, sizeof mf_fds);
    mf_log_next = 0;
    mf_log_count = 0;
    mf_unimpl_calls = 0;
    mf_rng = ((uint64_t)model_fuzz_seed + 1) * 0x9E3779B97F4A7C15ull;

    size_t disk_bytes = (size_t)sysconf(_SC_PAGESIZE);
    mf_total_blocks = disk_bytes / 512u - 1; /* minus the superblock */

    int r = sfs_format(MODEL_DISK, disk_bytes);
    CHECK(r == 0, "sfs_format(%zu) returned %d", disk_bytes, r);
    if (r != 0)
        return trace_ok;

    char namebuf[SFS_FILE_NAME_SIZE_LIMIT + 8];
    char namebuf2[SFS_FILE_NAME_SIZE_LIMIT + 8];
    char iobuf[2048];
    char payload[2048];

    for (int op = 1; op <= MF_DEFAULT_OPS; op++)
    {
        unsigned int kind = mf_rand_below(24);

        if (kind < 4)
        {
            /* open */
            const char *name = mf_pick_name(namebuf, sizeof namebuf);
            r = sfs_open(name);
            mf_note("%4d: open(\"%s\") = %d", op, name, r);
            if (strnlen(name, SFS_FILE_NAME_SIZE_LIMIT + 1) + 1 >
                SFS_FILE_NAME_SIZE_LIMIT)
            {
                if (r != -ENAMETOOLONG)
                    MF_FAIL("op %d: open(<%zu-char name>) should be "
                            "-ENAMETOOLONG, got %d",
                            op, strlen(name), r);
                continue;
            }
            int fi = mf_find(name);
            int fds_full = mf_open_fd_count() >= MF_MAX_FDS;
            if (fi < 0)
            {
                if (mf_used_blocks() + 1 > mf_total_blocks)
                {
                    if (r != -ENOSPC)
                        MF_FAIL("op %d: open(\"%s\") creating on a full disk "
                                "should be -ENOSPC, got %d", op, name, r);
                    continue;
                }
                if (fds_full)
                {
                    /* Handout behavior: the directory entry is created
                       before fd allocation fails, so the file exists
                       even though open() reports -EMFILE. */
                    if (r != -EMFILE)
                        MF_FAIL("op %d: open(\"%s\") with all %d fds in use "
                                "should be -EMFILE, got %d",
                                op, name, MF_MAX_FDS, r);
                    if (mf_create(name) < 0)
                        MF_FAIL("op %d: model bug: no free directory slot",
                                op);
                    continue;
                }
                if (r < 0)
                    MF_FAIL("op %d: open(\"%s\") should create the file, "
                            "got %d", op, name, r);
                fi = mf_create(name);
                if (fi < 0)
                    MF_FAIL("op %d: model bug: no free directory slot", op);
            }
            else
            {
                if (fds_full)
                {
                    if (r != -EMFILE)
                        MF_FAIL("op %d: open(\"%s\") with all %d fds in use "
                                "should be -EMFILE, got %d",
                                op, name, MF_MAX_FDS, r);
                    continue;
                }
                if (r < 0)
                    MF_FAIL("op %d: open(\"%s\") of an existing file failed "
                            "with %d", op, name, r);
            }
            if (r >= MF_MAX_FDS)
                MF_FAIL("op %d: open returned out-of-range fd %d", op, r);
            if (mf_fds[r].live)
                MF_FAIL("op %d: open returned fd %d, which is already open",
                        op, r);
            mf_fds[r].live = 1;
            mf_fds[r].file = fi;
            mf_fds[r].pos = 0;
        }
        else if (kind < 8)
        {
            /* close */
            int fd = mf_pick_fd();
            sfs_close(fd);
            mf_note("%4d: close(%d)", op, fd);
            if (mf_fd_valid(fd))
                mf_fds[fd].live = 0;
        }
        else if (kind < 12)
        {
            /* read */
            int fd = mf_pick_fd();
            unsigned int lensel = mf_rand_below(4);
            size_t len = lensel == 0 ? 0
                         : lensel == 1 ? 1 + mf_rand_below(64)
                         : lensel == 2 ? 400 + mf_rand_below(220)
                                       : 1000 + mf_rand_below(1048);
            ssize_t nr = sfs_read(fd, iobuf, len);
            mf_note("%4d: read(%d, len=%zu) = %zd", op, fd, len, nr);
            if (!mf_fd_valid(fd))
            {
                if (nr != -EBADF)
                    MF_FAIL("op %d: read(bad fd %d) should be -EBADF, got %zd",
                            op, fd, nr);
                continue;
            }
            struct mf_fd *m = &mf_fds[fd];
            struct mf_file *f = &mf_files[m->file];
            size_t want = f->size - m->pos;
            if (len < want)
                want = len;
            if (nr != (ssize_t)want)
                MF_FAIL("op %d: read(fd %d, len=%zu) at pos %zu of %zu-byte "
                        "file should return %zu, got %zd",
                        op, fd, len, m->pos, f->size, want, nr);
            if (want > 0 && memcmp(iobuf, mf_data[m->file] + m->pos, want))
                MF_FAIL("op %d: read(fd %d) data mismatch at pos %zu "
                        "(%zu bytes)", op, fd, m->pos, want);
            m->pos += want;
        }
        else if (kind < 16)
        {
            /* write */
            int fd = mf_pick_fd();
            unsigned int lensel = mf_rand_below(4);
            size_t len = lensel == 0 ? 0
                         : lensel == 1 ? 1 + mf_rand_below(64)
                         : lensel == 2 ? 400 + mf_rand_below(220)
                                       : 1000 + mf_rand_below(1048);
            if (mf_fd_valid(fd))
            {
                /* Keep files within the model's per-file buffer; the cap
                   equals a full 4K-page disk so nothing is lost. */
                size_t room = MF_FILE_CAP - mf_fds[fd].pos;
                if (len > room)
                    len = room;
            }
            for (size_t i = 0; i < len; i++)
                payload[i] = (char)('a' + (((size_t)op + i) % 26u));
            ssize_t nw = sfs_write(fd, payload, len);
            mf_note("%4d: write(%d, len=%zu) = %zd", op, fd, len, nw);
            if (!mf_fd_valid(fd))
            {
                if (nw != -EBADF)
                    MF_FAIL("op %d: write(bad fd %d) should be -EBADF, "
                            "got %zd", op, fd, nw);
                continue;
            }
            struct mf_fd *m = &mf_fds[fd];
            struct mf_file *f = &mf_files[m->file];
            size_t endPos = m->pos + len;
            size_t have = mf_blocks_of(f->size);
            size_t need = mf_blocks_of(endPos == 0 ? f->size : endPos);
            size_t addl = need > have ? need - have : 0;
            if (addl > 0 &&
                mf_used_blocks() + addl > mf_total_blocks)
            {
                if (nw != -ENOSPC)
                    MF_FAIL("op %d: write(fd %d, len=%zu) needing %zu new "
                            "block(s) on a full disk should be -ENOSPC, "
                            "got %zd", op, fd, len, addl, nw);
                continue;
            }
            if (nw != (ssize_t)len)
                MF_FAIL("op %d: write(fd %d, len=%zu) should write all "
                        "bytes, got %zd", op, fd, len, nw);
            if (len > 0)
            {
                memcpy(mf_data[m->file] + m->pos, payload, len);
                m->pos = endPos;
                if (endPos > f->size)
                    f->size = endPos;
            }
        }
        else if (kind < 18)
        {
            /* seek */
            int fd = mf_pick_fd();
            ssize_t delta;
            unsigned int dsel = mf_rand_below(8);
            if (dsel == 0)
                delta = 9999;
            else if (dsel == 1)
                delta = -9999;
            else
                delta = (ssize_t)mf_rand_below(3201) - 1600;
            ssize_t p = sfs_seek(fd, delta);
            mf_note("%4d: seek(%d, %zd) = %zd", op, fd, delta, p);
            if (p == -ENOSYS)
            {
                mf_unimpl_calls++;
                continue;
            }
            if (!mf_fd_valid(fd))
            {
                if (p != -EBADF)
                    MF_FAIL("op %d: seek(bad fd %d) should be -EBADF, "
                            "got %zd", op, fd, p);
                continue;
            }
            struct mf_fd *m = &mf_fds[fd];
            struct mf_file *f = &mf_files[m->file];
            ssize_t want = (ssize_t)m->pos + delta;
            if (want < 0)
                want = 0;
            if ((size_t)want > f->size)
                want = (ssize_t)f->size;
            if (p != want)
                MF_FAIL("op %d: seek(fd %d, %zd) from pos %zu of %zu-byte "
                        "file should return %zd, got %zd",
                        op, fd, delta, m->pos, f->size, want, p);
            m->pos = (size_t)want;
        }
        else if (kind < 20)
        {
            /* getpos */
            int fd = mf_pick_fd();
            ssize_t p = sfs_getpos(fd);
            mf_note("%4d: getpos(%d) = %zd", op, fd, p);
            if (p == -ENOSYS)
            {
                mf_unimpl_calls++;
                continue;
            }
            if (!mf_fd_valid(fd))
            {
                if (p != -EBADF)
                    MF_FAIL("op %d: getpos(bad fd %d) should be -EBADF, "
                            "got %zd", op, fd, p);
                continue;
            }
            if (p != (ssize_t)mf_fds[fd].pos)
                MF_FAIL("op %d: getpos(fd %d) should be %zu, got %zd",
                        op, fd, mf_fds[fd].pos, p);
        }
        else if (kind < 21)
        {
            /* remove */
            const char *name = mf_pick_name(namebuf, sizeof namebuf);
            r = sfs_remove(name);
            mf_note("%4d: remove(\"%s\") = %d", op, name, r);
            if (strnlen(name, SFS_FILE_NAME_SIZE_LIMIT + 1) + 1 >
                SFS_FILE_NAME_SIZE_LIMIT)
            {
                if (r != -ENAMETOOLONG)
                    MF_FAIL("op %d: remove(<long name>) should be "
                            "-ENAMETOOLONG, got %d", op, r);
                continue;
            }
            int fi = mf_find(name);
            if (fi < 0)
            {
                if (r != -ENOENT)
                    MF_FAIL("op %d: remove(\"%s\") of a missing file should "
                            "be -ENOENT, got %d", op, name, r);
                continue;
            }
            if (mf_file_has_open_fd(fi))
            {
                if (r != -EBUSY)
                    MF_FAIL("op %d: remove(\"%s\") of an open file should be "
                            "-EBUSY, got %d", op, name, r);
                continue;
            }
            if (r != 0)
                MF_FAIL("op %d: remove(\"%s\") should succeed, got %d",
                        op, name, r);
            mf_files[fi].live = 0;
        }
        else if (kind < 22)
        {
            /* rename; skip generations with unspecified semantics */
            const char *old_name = mf_pick_name(namebuf, sizeof namebuf);
            const char *new_name = mf_pick_name(namebuf2, sizeof namebuf2);
            int old_long = strnlen(old_name, SFS_FILE_NAME_SIZE_LIMIT + 1) +
                               1 > SFS_FILE_NAME_SIZE_LIMIT;
            int new_long = strnlen(new_name, SFS_FILE_NAME_SIZE_LIMIT + 1) +
                               1 > SFS_FILE_NAME_SIZE_LIMIT;
            if (!old_long && !new_long)
            {
                if (strcmp(old_name, new_name) == 0)
                    continue;
                int oi = mf_find(old_name);
                int ni = mf_find(new_name);
                if ((oi >= 0 && mf_file_has_open_fd(oi)) ||
                    (ni >= 0 && mf_file_has_open_fd(ni)))
                    continue;
            }
            r = sfs_rename(old_name, new_name);
            mf_note("%4d: rename(\"%.10s\", \"%.10s\") = %d",
                    op, old_name, new_name, r);
            if (r == -ENOSYS)
            {
                mf_unimpl_calls++;
                continue;
            }
            if (old_long || new_long)
            {
                if (r != -ENAMETOOLONG)
                    MF_FAIL("op %d: rename with an over-limit name should be "
                            "-ENAMETOOLONG, got %d", op, r);
                continue;
            }
            int oi = mf_find(old_name);
            int ni = mf_find(new_name);
            if (oi < 0)
            {
                if (r != -ENOENT)
                    MF_FAIL("op %d: rename(\"%s\", ...) of a missing file "
                            "should be -ENOENT, got %d", op, old_name, r);
                continue;
            }
            if (r != 0)
                MF_FAIL("op %d: rename(\"%s\", \"%s\") should succeed, "
                        "got %d", op, old_name, new_name, r);
            if (ni >= 0)
                mf_files[ni].live = 0;
            snprintf(mf_files[oi].name, sizeof mf_files[oi].name, "%s",
                     new_name);
        }
        else if (kind < 23)
        {
            /* full directory listing */
            sfs_list_cookie cookie = NULL;
            char seen[MF_MAX_FILES] = {0};
            char lname[SFS_FILE_NAME_SIZE_LIMIT];
            int status;
            int count = 0;
            while ((status = sfs_list(&cookie, lname, sizeof lname)) == 0)
            {
                count++;
                if (count > MF_MAX_FILES)
                    break;
                int fi = mf_find(lname);
                if (fi < 0)
                    MF_FAIL("op %d: list returned \"%s\", which should not "
                            "exist", op, lname);
                if (seen[fi])
                    MF_FAIL("op %d: list returned \"%s\" twice", op, lname);
                seen[fi] = 1;
            }
            mf_note("%4d: list -> %d file(s), status %d", op, count, status);
            if (status != 1)
                MF_FAIL("op %d: list should end with +1, got %d", op, status);
            int live = 0;
            for (int i = 0; i < MF_MAX_FILES; i++)
                live += mf_files[i].live;
            if (count != live)
                MF_FAIL("op %d: list found %d file(s), model has %d",
                        op, count, live);
        }
        else
        {
            /* explicit bad-fd probes */
            int fd = mf_rand_below(2) ? -1 : MF_MAX_FDS;
            ssize_t nr = sfs_read(fd, iobuf, 8);
            ssize_t nw = sfs_write(fd, "x", 1);
            mf_note("%4d: badfd(%d) read=%zd write=%zd", op, fd, nr, nw);
            if (nr != -EBADF || nw != -EBADF)
                MF_FAIL("op %d: read/write on fd %d should be -EBADF, "
                        "got %zd/%zd", op, fd, nr, nw);
            sfs_close(fd);
        }

        if (op % MF_CHECKPOINT_EVERY == 0 || op == MF_DEFAULT_OPS)
        {
            /* checkpoint: close everything, unmount, fsck, remount */
            for (int i = 0; i < MF_MAX_FDS; i++)
            {
                if (mf_fds[i].live)
                {
                    sfs_close(i);
                    mf_fds[i].live = 0;
                }
            }
            r = sfs_unmount();
            mf_note("%4d: checkpoint unmount = %d", op, r);
            if (r != 0)
                MF_FAIL("op %d: checkpoint unmount should succeed, got %d",
                        op, r);
            char detail[384];
            int rc = run_fsck_capture(MODEL_DISK, detail, sizeof detail);
            if (rc != 0)
                MF_FAIL("op %d: checkpoint sfs-fsck failed (exit %d): %s",
                        op, rc, detail[0] ? detail : "no output");
            r = sfs_mount(MODEL_DISK);
            if (r != 0)
                MF_FAIL("op %d: checkpoint remount failed: %d", op, r);
        }
    }

mf_done:
    for (int i = 0; i < MF_MAX_FDS; i++)
        sfs_close(i);
    sfs_unmount();
    if (trace_ok)
    {
        unlink(MODEL_DISK);
        if (mf_unimpl_calls > 0)
            printf("  (model fuzz: %d call(s) hit -ENOSYS stubs and were "
                   "treated as no-ops)\n", mf_unimpl_calls);
    }
    return trace_ok;
}

#undef MF_FAIL

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
            continue;
        for (int k = 0; k < PERF_IO_ROUNDS; k++)
        {
            char data[64];
            int len = snprintf(data, sizeof data, "iter-%d-%d-%d", id, i, k);
            sfs_write(fd, data, (size_t)len);
            sfs_seek(fd, -sfs_getpos(fd));
            char buf[64];
            sfs_read(fd, buf, sizeof buf);
        }
        sfs_close(fd);
    }
    return NULL;
}

static double elapsed_sec(struct timespec *start, struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/* Build the preferred path to .perf_baseline: next to the currently running
   executable (via /proc/self/exe), so `./test-sfs` still finds the file when
   invoked from a different cwd. Writes at most `cap` bytes into `out` and
   returns 0 on success, -1 if the path did not fit. Callers should try a
   plain "./.perf_baseline" fallback if the exe-relative file does not exist. */
static int resolve_baseline_path(char *out, size_t cap)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0 && (size_t)n < sizeof(exe))
    {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash)
        {
            *slash = '\0';
            int k = snprintf(out, cap, "%s/.perf_baseline", exe);
            if (k > 0 && (size_t)k < cap) return 0;
        }
    }
    int k = snprintf(out, cap, ".perf_baseline");
    return (k > 0 && (size_t)k < cap) ? 0 : -1;
}

struct baseline_info
{
    double ops;
    time_t mtime;                  /* 0 if unavailable */
    char   path[PATH_MAX];
    char   disk_dir[PATH_MAX];     /* SFS_DISK_DIR at calibration time; "(unset)" if absent */
    char   workload[32];           /* WORKLOAD= tag; "v1" for pre-tag files */
};

/* Populate `info` from .perf_baseline. Tries the exe-relative path first,
   then falls back to "./.perf_baseline". Returns 0 on success, -1 if the
   file is missing, unreadable, or holds a non-finite / non-positive value. */
static int load_baseline(struct baseline_info *info)
{
    info->ops = -1.0;
    info->mtime = 0;
    info->path[0] = '\0';
    snprintf(info->disk_dir, sizeof(info->disk_dir), "(unset)");
    /* Files written before the workload tag existed are all from the
       original (v1) benchmark shape. */
    snprintf(info->workload, sizeof(info->workload), "v1");

    if (resolve_baseline_path(info->path, sizeof(info->path)) != 0)
        return -1;

    FILE *f = fopen(info->path, "r");
    if (!f && strcmp(info->path, ".perf_baseline") != 0)
    {
        snprintf(info->path, sizeof(info->path), ".perf_baseline");
        f = fopen(info->path, "r");
    }
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
               "           see writeup section 5.4 (Docker/Windows) for mitigation.\n");
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
             "-I. -o %s local/test-sfs.c local/test-report.c "
             "sfs-disk.c sfs-support.c 2>&1",
             tsan_bin);

    int rc = system(compile_cmd);
    if (rc != 0)
    {
        printf("  (skipped -- TSan compilation failed, gcc may not support "
               "-fsanitize=thread)\n");
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
            result = TSAN_UNAVAILABLE;
            break;
        }

        if (log_contains(tsan_log, "WARNING: ThreadSanitizer: data race"))
        {
            printf("  DATA RACE DETECTED (schedule-fuzz seed %u) -- "
                   "Category C score set to 0\n", seed);
            printf("  Race stacks were suppressed; SFS_Lab_Writeup.md shows "
                   "how to rerun TSan to print them.\n");
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

/* Run the X traces, each forked with a timeout.  In the default full
   run they execute quietly and show "--" when not achieved, so an
   untouched starter is not flooded with FAIL detail for optional work;
   --x-only prints the full diagnostics. */
static int run_x_traces(int show_detail)
{
    struct trace_entry xs[] = {
        {"X00", "empty_file_blocks", trace_X00},
        {"X01", "dir_expansion", trace_X01},
        {"X02", "unix_remove", trace_X02},
    };
    const int n = (int)(sizeof xs / sizeof xs[0]);

    printf("\nCategory X (Optional Challenges, not scored):\n");
    int achieved = 0;
    for (int i = 0; i < n; i++)
    {
        int saved_quiet = quiet_mode;
        if (!show_detail)
            quiet_mode = 1;
        atomic_store_explicit(&trace_fail_count, 0, memory_order_relaxed);
        char *buf = NULL;
        size_t buflen = 0;
        int ok = run_trace_with_timeout(xs[i].id, xs[i].fn, &buf, &buflen);
        quiet_mode = saved_quiet;
        achieved += ok;
        printf("  %s %-24s %s  [optional]\n", xs[i].id, xs[i].name,
               ok ? "PASS" : "--");
        fflush(stdout);
        if (show_detail && buf && buflen > 0)
            fwrite(buf, 1, buflen, stderr);
        fflush(stderr);
        free(buf);
    }
    printf("  Achieved: %d/%d (does not affect the graded score)\n",
           achieved, n);
    return achieved;
}

static int run_category(const char *label, struct trace_entry *traces, int n)
{
    printf("\nCategory %s:\n", label);
    /* Concurrent and stress diagnostics run in a forked child so deadlocks,
       aborts, and heap corruption in the student's implementation do not take
       down the autograder/reporting process. */
    int use_timeout = (label[0] == 'C' || strncmp(label, "Stress", 6) == 0 ||
                       strncmp(label, "Model", 5) == 0);
    int passed = 0;
    for (int i = 0; i < n; i++)
    {
        int ok;
        char *buf = NULL;
        size_t buflen = 0;
        FILE *ms = NULL;

        atomic_store_explicit(&trace_fail_count, 0, memory_order_relaxed);
        if (use_timeout)
        {
            /* Forked child's stderr is captured via pipe; the returned buf
               holds FAIL details (and TIMEOUT/CRASH if any) for the parent
               to print after the summary line, same ordering as A/B. */
            ok = run_trace_with_timeout(traces[i].id, traces[i].fn,
                                        &buf, &buflen);
        }
        else
        {
            ms = open_memstream(&buf, &buflen);
            check_stream = ms ? ms : stderr;
            ok = traces[i].fn();
            trace_fail_tail(check_stream);
            if (!ok)
            {
                char *hint = keep_failure_images(traces[i].id);
                if (hint)
                {
                    fputs(hint, check_stream);
                    free(hint);
                }
            }
            check_stream = stderr;
            if (ms) fclose(ms);
        }
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

static int silence_stdout(int *saved_stdout)
{
    fflush(stdout);
    *saved_stdout = dup(STDOUT_FILENO);
    if (*saved_stdout < 0)
        return -1;

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull < 0)
    {
        close(*saved_stdout);
        *saved_stdout = -1;
        return -1;
    }

    if (dup2(devnull, STDOUT_FILENO) < 0)
    {
        close(devnull);
        close(*saved_stdout);
        *saved_stdout = -1;
        return -1;
    }
    close(devnull);
    return 0;
}

static void restore_stdout(int saved_stdout)
{
    fflush(stdout);
    if (saved_stdout >= 0)
    {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
    }
}

int main(int argc, char *argv[])
{
    init_disk_paths();

    int mode_tsan_only = 0;
    int mode_perf_only = 0;
    int mode_smoke_only = 0;
    int mode_concurrency_only = 0;
    int mode_stress_only = 0;
    int mode_model_fuzz = 0;
    int mode_x_only = 0;
    int mode_list_traces = 0;
    int mode_json = 0;
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
        else if (strcmp(argv[i], "--smoke-only") == 0)
            mode_smoke_only = 1;
        else if (strcmp(argv[i], "--concurrency-only") == 0)
            mode_concurrency_only = 1;
        else if (strcmp(argv[i], "--stress-only") == 0)
            mode_stress_only = 1;
        else if (strcmp(argv[i], "--list-traces") == 0)
            mode_list_traces = 1;
        else if (strcmp(argv[i], "--json") == 0)
            mode_json = 1;
        else if (strcmp(argv[i], "--sched-fuzz") == 0)
            sched_fuzz_seed = 1;
        else if (strncmp(argv[i], "--sched-fuzz=", 13) == 0)
            sched_fuzz_seed = (unsigned int)strtoul(argv[i] + 13, NULL, 10);
        else if (strcmp(argv[i], "--x-only") == 0)
            mode_x_only = 1;
        else if (strcmp(argv[i], "--model-fuzz") == 0)
            mode_model_fuzz = 1;
        else if (strncmp(argv[i], "--model-fuzz=", 13) == 0)
        {
            mode_model_fuzz = 1;
            model_fuzz_seed =
                (unsigned int)strtoul(argv[i] + 13, NULL, 10);
        }
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

    keep_failed_disks = !mode_json && !mode_tsan_only;
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
            {"C02", "rw_mix_storm", trace_C02},
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
        {"C02", "rw_mix_storm", trace_C02},
    };

    struct trace_entry stress_traces[] = {
        {"S00", "dir_churn", trace_stress_dir_churn},
        {"S01", "rename_atomic", trace_stress_rename_atomic},
    };
    const int n_stress = (int)(sizeof stress_traces / sizeof stress_traces[0]);

    if (mode_list_traces)
    {
        printf("category\tid\tname\tpoints\n");
        for (int i = 0; i < 5; i++)
            printf("A\t%s\t%s\t1\n", cat_a[i].id, cat_a[i].name);
        for (int i = 0; i < 4; i++)
            printf("B\t%s\t%s\t1\n", cat_b[i].id, cat_b[i].name);
        for (int i = 0; i < 3; i++)
            printf("C\t%s\t%s\t1\n", cat_c[i].id, cat_c[i].name);
        printf("Perf\tbenchmark\tthroughput\t10\n");
        return 0;
    }

    int saved_stdout = -1;
    if (mode_json)
    {
        quiet_mode = 1;
        if (silence_stdout(&saved_stdout) != 0)
        {
            fprintf(stderr, "warning: could not silence stdout for JSON mode\n");
            saved_stdout = -1;
        }
    }

    printf("========================================\n");
    printf("        SFS Lab Autograder\n");
    printf("========================================\n");

    if (mode_smoke_only)
    {
        int smoke = run_category("Smoke", cat_a, 2);
        unlink(DISK_NAME);
        return (smoke == 2) ? 0 : 1;
    }

    if (mode_concurrency_only)
    {
        int c = run_category("C (Concurrent Correctness)", cat_c, 3);
        unlink(DISK_NAME);
        cleanup_concurrency_disks();
        if (mode_json)
        {
            restore_stdout(saved_stdout);
            test_report_print_trace_set_json("concurrency", c, 3, 1);
        }
        return (c == 3) ? 0 : 1;
    }

    if (mode_x_only)
    {
        int x = run_x_traces(1);
        unlink(DISK_NAME);
        if (mode_json)
        {
            restore_stdout(saved_stdout);
            test_report_print_trace_set_json("x_traces", x, 3, 0);
        }
        return (x == 3) ? 0 : 1;
    }

    if (mode_model_fuzz)
    {
        printf("  (differential model fuzz, seed %u, %d ops; rerun with "
               "--model-fuzz=%u)\n",
               model_fuzz_seed, MF_DEFAULT_OPS, model_fuzz_seed);
        struct trace_entry mf_traces[] = {
            {"M00", "model_fuzz", trace_model_fuzz},
        };
        int mf_ok = run_category("Model Fuzz (differential, diagnostic)",
                                 mf_traces, 1);
        unlink(MODEL_DISK);
        if (mode_json)
        {
            restore_stdout(saved_stdout);
            test_report_print_trace_set_json("model_fuzz", mf_ok, 1, 0);
        }
        return (mf_ok == 1) ? 0 : 1;
    }

    if (mode_stress_only)
    {
        int stress = run_category("Stress Diagnostics", stress_traces,
                                  n_stress);
        unlink(DISK_NAME);
        cleanup_concurrency_disks();
        if (mode_json)
        {
            restore_stdout(saved_stdout);
            test_report_print_trace_set_json("stress_diagnostics", stress,
                                             n_stress, 0);
        }
        return (stress == n_stress) ? 0 : 1;
    }

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
    int perf_ran = 0;
    if (correctness < 12)
    {
        printf("  (skipped -- correctness tests must all pass first)\n");
        printf("  Score: 0/10\n");
    }
    else
    {
        perf_ran = 1;
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

    /* Optional challenges run after the graded scoreboard; they never
       change the score or the exit status. */
    int x = run_x_traces(0);

    unlink(DISK_NAME);
    if (mode_json)
    {
        restore_stdout(saved_stdout);
        test_report_print_score_json(a, b, c, tsan, perf, perf_ran, x);
    }
    return (correctness == 12) ? 0 : 1;
}
