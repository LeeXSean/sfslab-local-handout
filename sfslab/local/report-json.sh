#!/bin/sh
set -u

strict=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --strict)
            strict=1
            shift
            ;;
        --)
            shift
            break
            ;;
        *)
            echo "report-json: unknown option: $1" >&2
            exit 2
            ;;
    esac
done

grader_json=$(mktemp)
grader_err=$(mktemp)
trace_json=$(mktemp)
trace_err=$(mktemp)
stress_json=$(mktemp)
stress_err=$(mktemp)
model_json=$(mktemp)
model_err=$(mktemp)
trap 'rm -f "$grader_json" "$grader_err" "$trace_json" "$trace_err" "$stress_json" "$stress_err" "$model_json" "$model_err"' EXIT HUP INT TERM

./test-sfs --json > "$grader_json" 2> "$grader_err"
grader_status=$?

./test-sfs --json --stress-only > "$stress_json" 2> "$stress_err"
stress_status=$?

# Fixed seed so report runs are reproducible across machines.
./test-sfs --json --model-fuzz=1 > "$model_json" 2> "$model_err"
model_status=$?

trace_available=true
trace_status=0
if sh local/build-lua-runner.sh > /dev/null 2> "$trace_err"; then
    sh local/run-lua-traces.sh --json > "$trace_json" 2> "$trace_err"
    trace_status=$?
else
    trace_available=false
    trace_status=77
fi

printf '{\n'
printf '  "local_autograder": {\n'
printf '    "exit_status": %d,\n' "$grader_status"
if [ -s "$grader_json" ]; then
    printf '    "result": '
    sed '1s/^//; 2,$s/^/    /' "$grader_json"
    printf '\n'
else
    printf '    "result": null\n'
fi
printf '  },\n'
printf '  "official_style_traces": {\n'
printf '    "available": %s,\n' "$trace_available"
printf '    "exit_status": %d,\n' "$trace_status"
if [ "$trace_available" = true ] && [ -s "$trace_json" ]; then
    printf '    "result": '
    sed '1s/^//; 2,$s/^/    /' "$trace_json"
    printf '\n'
else
    printf '    "result": null\n'
fi
printf '  },\n'
printf '  "stress_diagnostics": {\n'
printf '    "available": true,\n'
printf '    "exit_status": %d,\n' "$stress_status"
printf '    "graded": false,\n'
if [ -s "$stress_json" ]; then
    printf '    "result": '
    sed '1s/^//; 2,$s/^/    /' "$stress_json"
    printf '\n'
else
    printf '    "result": null\n'
fi
printf '  },\n'
printf '  "model_fuzz": {\n'
printf '    "available": true,\n'
printf '    "exit_status": %d,\n' "$model_status"
printf '    "graded": false,\n'
printf '    "seed": 1,\n'
if [ -s "$model_json" ]; then
    printf '    "result": '
    sed '1s/^//; 2,$s/^/    /' "$model_json"
    printf '\n'
else
    printf '    "result": null\n'
fi
printf '  },\n'
printf '  "combined": {\n'
printf '    "graded_total_uses": "local_autograder.result.total",\n'
printf '    "trace_coverage_is_diagnostic": true,\n'
printf '    "stress_diagnostics_is_diagnostic": true,\n'
printf '    "model_fuzz_is_diagnostic": true,\n'
if [ "$grader_status" -eq 0 ]; then
    printf '    "graded_passed": true,\n'
else
    printf '    "graded_passed": false,\n'
fi
if { [ "$trace_status" -eq 0 ] || [ "$trace_status" -eq 77 ]; } &&
   [ "$stress_status" -eq 0 ] && [ "$model_status" -eq 0 ]; then
    printf '    "diagnostics_passed": true\n'
else
    printf '    "diagnostics_passed": false\n'
fi
printf '  }\n'
printf '}\n'

if [ "$strict" -eq 1 ]; then
    exit "$grader_status"
fi

if [ -s "$grader_json" ] && [ -s "$stress_json" ] && [ -s "$model_json" ] &&
   { [ "$trace_available" != true ] || [ -s "$trace_json" ]; }; then
    exit 0
fi

exit 1
