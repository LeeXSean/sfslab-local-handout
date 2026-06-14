#include "test-report.h"

#include <stdio.h>

const char *tsan_result_name(enum tsan_result result)
{
    switch (result)
    {
    case TSAN_SKIPPED: return "skipped";
    case TSAN_CLEAN: return "clean";
    case TSAN_RACE: return "race_detected";
    case TSAN_TRACE_FAILED: return "trace_failed";
    case TSAN_UNAVAILABLE: return "unavailable";
    case TSAN_TIMEOUT: return "timeout";
    }
    return "unknown";
}

int tsan_result_ran(enum tsan_result result)
{
    return result == TSAN_CLEAN || result == TSAN_RACE ||
           result == TSAN_TRACE_FAILED || result == TSAN_TIMEOUT;
}

const char *tsan_clean_json(enum tsan_result result)
{
    if (result == TSAN_CLEAN)
        return "true";
    if (tsan_result_ran(result))
        return "false";
    return "null";
}

void test_report_print_score_json(int a, int b, int c,
                                  enum tsan_result tsan,
                                  int perf, int perf_ran, int x)
{
    int correctness = a + b + c;
    int total = correctness + perf;

    printf("{\n");
    printf("  \"correctness\": {\"score\": %d, \"max\": 12},\n",
           correctness);
    printf("  \"categories\": {\n");
    printf("    \"A\": {\"score\": %d, \"max\": 5},\n", a);
    printf("    \"B\": {\"score\": %d, \"max\": 4},\n", b);
    printf("    \"C\": {\"score\": %d, \"max\": 3, \"tsan_ran\": %s, "
           "\"tsan_clean\": %s, \"tsan_status\": \"%s\"}\n",
           c, tsan_result_ran(tsan) ? "true" : "false",
           tsan_clean_json(tsan), tsan_result_name(tsan));
    printf("  },\n");
    printf("  \"performance\": {\"score\": %d, \"max\": 10, \"ran\": %s},\n",
           perf, perf_ran ? "true" : "false");
    printf("  \"total\": {\"score\": %d, \"max\": 22},\n", total);
    printf("  \"x_traces\": {\"score\": %d, \"max\": 3, \"graded\": false},\n",
           x);
    printf("  \"style\": {\"max\": 4, \"manual\": true},\n");
    printf("  \"passed\": %s\n", correctness == 12 ? "true" : "false");
    printf("}\n");
}

void test_report_print_trace_set_json(const char *name, int score, int max,
                                      int graded)
{
    printf("{\n");
    printf("  \"%s\": {\"score\": %d, \"max\": %d, \"graded\": %s, "
           "\"passed\": %s}\n",
           name, score, max, graded ? "true" : "false",
           score == max ? "true" : "false");
    printf("}\n");
}
