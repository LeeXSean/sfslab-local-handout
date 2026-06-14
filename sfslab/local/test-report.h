#ifndef SFS_TEST_REPORT_H
#define SFS_TEST_REPORT_H

enum tsan_result
{
    TSAN_SKIPPED,
    TSAN_CLEAN,
    TSAN_RACE,
    TSAN_TRACE_FAILED,
    TSAN_UNAVAILABLE,
    TSAN_TIMEOUT,
};

const char *tsan_result_name(enum tsan_result result);
int tsan_result_ran(enum tsan_result result);
const char *tsan_clean_json(enum tsan_result result);

void test_report_print_score_json(int a, int b, int c,
                                  enum tsan_result tsan,
                                  int perf, int perf_ran, int x);
void test_report_print_trace_set_json(const char *name, int score, int max,
                                      int graded);

#endif
