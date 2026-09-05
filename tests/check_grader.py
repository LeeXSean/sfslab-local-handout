"""Run with python3 tests/check_grader.py after make. No student solution needed."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
handout = root / 'sfslab'
with tempfile.TemporaryDirectory(prefix='sfs-check-') as tmp:
    tmp = Path(tmp)
    env = dict(os.environ, SFS_DISK_DIR=str(tmp), SFS_KEEP_FAILED_DISKS='0')
    names = ['test.img', 'test_perf.img', 'test_conc_C00.img', 'fail_A00_test.img']
    for name in names:
        (tmp / name).write_bytes(b'user image: preserve me')
    for args, expected in [(['test-sfs', '-q'], 1),
                           (['test-sfs-baseline', '--tsan-only'], 0)]:
        result = subprocess.run([str(handout / args[0]), *args[1:]],
                                cwd=handout, env=env, capture_output=True,
                                text=True, timeout=120)
        assert result.returncode == expected, result.stdout + result.stderr
        if args[0] == 'test-sfs':
            assert 'Lab correctness:' in result.stdout
            assert 'Local benchmark total:' not in result.stdout
        for name in names:
            assert (tmp / name).read_bytes() == b'user image: preserve me'
    assert not list(tmp.glob('sfs-test.*')), 'successful/quiet runs leaked images'
    source = tmp / 'check.c'
    source.write_text(r'''
#define main driver_main
#include "test-sfs.c"
#undef main
#include <assert.h>
static int noisy_trace(void)
{
    char noise[4096] = {0};
    for (;;)
        (void)write(STDERR_FILENO, noise, sizeof noise);
    return 1;
}
static pthread_barrier_t gate;
static void *worker(void *arg)
{
    c_api_enter();
    pthread_barrier_wait(&gate); /* A driver lock would deadlock here. */
    CHECK(0, "intentional concurrent failure");
    c_api_leave();
    return NULL;
}
int main(void)
{
    _Static_assert(_Generic(&trace_ok, _Atomic int *: 1, default: 0),
                   "failure flag must be atomic");
    quiet_mode = 1;
    trace_ok = 1;
    pthread_t a, b;
    assert(pthread_barrier_init(&gate, NULL, 2) == 0);
    assert(pthread_create(&a, NULL, worker, NULL) == 0);
    assert(pthread_create(&b, NULL, worker, NULL) == 0);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    assert(!trace_ok && trace_fail_count == 2);
    char *output;
    size_t length;
    assert(run_trace_with_timeout("noise", noisy_trace, &output, &length) == 0);
    free(output);
    return 0;
}
'''.replace('#include "test-sfs.c"',
               (handout / 'test-sfs.c').read_text().replace(
                   '#define TRACE_TIMEOUT_SEC 30', '#define TRACE_TIMEOUT_SEC 1')))
    binary = tmp / 'check'
    subprocess.run(['gcc', '-std=c11', '-D_GNU_SOURCE=1', '-pthread',
                    '-I', str(handout), str(source),
                    str(handout / 'sfs-baseline-ref.c'),
                    str(handout / 'sfs-support.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('Grader regression checks passed.')
