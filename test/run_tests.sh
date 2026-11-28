#!/usr/bin/env sh
set -u

test_dir=$(cd "$(dirname "$0")" && pwd)
root_dir=$(dirname "$test_dir")
compiler=${1:-${VASTNOVA:-$root_dir/vastnova}}

case "$compiler" in
    /*) ;;
    *) compiler="$PWD/$compiler" ;;
esac
if [ ! -x "$compiler" ]; then
    echo "Compiler not found or not executable: $compiler" >&2
    echo "Build it first with ./build.sh, or pass the compiler path." >&2
    exit 2
fi

temp_root="$test_dir/temp"
mkdir -p "$temp_root" || {
    echo "Cannot create temporary directory: $temp_root" >&2
    exit 2
}
work_dir=$(mktemp -d "$temp_root/run.XXXXXX") || {
    echo "Cannot create temporary directory under: $temp_root" >&2
    exit 2
}
trap 'rm -f "${exec_copy:-}"; rm -rf "$work_dir"' EXIT HUP INT TERM

passed=0
failed=0

report_failure() {
    echo "FAIL: $1"
    shift
    for line in "$@"; do
        [ -n "$line" ] && echo "  $line"
    done
    failed=$((failed + 1))
}

run_binary() {
    binary=$1
    output=$2
    stdin_file=$3
    if command -v timeout >/dev/null 2>&1; then
        if [ -n "$stdin_file" ]; then
            timeout 10 "$binary" <"$stdin_file" >"$output" 2>&1
        else
            timeout 10 "$binary" >"$output" 2>&1
        fi
    else
        if [ -n "$stdin_file" ]; then
            "$binary" <"$stdin_file" >"$output" 2>&1
        else
            "$binary" >"$output" 2>&1
        fi
    fi
}

run_program_test() {
    source=$1
    expected=$2
    name=$(basename "$source" .vn)
    case_dir="$work_dir/$name"
    mkdir -p "$case_dir"
    cp "$source" "$case_dir/$name.vn"

    if ! (cd "$root_dir" && "$compiler" "$case_dir/$name.vn" "$case_dir/$name") \
            >"$case_dir/build.log" 2>&1; then
        report_failure "$name (compilation failed)" \
            "$(tail -n 3 "$case_dir/build.log")"
        return
    fi

    input_file="${source%.vn}.stdin"
    [ -f "$input_file" ] || input_file=""

    run_binary "$case_dir/$name" "$case_dir/output" "$input_file"
    status=$?
    if [ "$status" -eq 126 ]; then
        exec_dir=${TMPDIR:-${HOME:-/tmp}}
        exec_copy="$exec_dir/vastnova-test-$$-$name"
        if cp "$case_dir/$name" "$exec_copy" 2>/dev/null; then
            chmod +x "$exec_copy" 2>/dev/null
            run_binary "$exec_copy" "$case_dir/output" "$input_file"
            status=$?
            rm -f "$exec_copy"
        fi
    fi
    if [ "$status" -ne 0 ]; then
        report_failure "$name (exit status $status)" \
            "$(tail -n 3 "$case_dir/output")"
        return
    fi

    if diff -u "$expected" "$case_dir/output" >"$case_dir/diff.txt"; then
        echo "PASS: $name"
        passed=$((passed + 1))
    else
        report_failure "$name (unexpected output)" \
            "$(tail -n 20 "$case_dir/diff.txt")"
    fi
}

run_error_test() {
    source=$1
    expected=$2
    name=$(basename "$source" .vn)
    case_dir="$work_dir/error_$name"
    mkdir -p "$case_dir"
    cp "$source" "$case_dir/$name.vn"

    (cd "$root_dir" && "$compiler" "$case_dir/$name.vn" --no-run) \
        >"$case_dir/output.log" 2>&1
    status=$?
    if [ "$status" -eq 0 ]; then
        report_failure "$name (expected compilation to fail)"
        return
    fi
    if grep -F -q -f "$expected" "$case_dir/output.log"; then
        echo "PASS: $name (error)"
        passed=$((passed + 1))
    else
        report_failure "$name (missing expected diagnostic)" \
            "$(tail -n 5 "$case_dir/output.log")"
    fi
}

echo "Using compiler: $compiler"
echo

for source in "$test_dir"/cases/*.vn; do
    [ -e "$source" ] || continue
    expected="${source%.vn}.expected"
    if [ ! -f "$expected" ]; then
        report_failure "$(basename "$source") (missing .expected file)"
        continue
    fi
    run_program_test "$source" "$expected"
done

for source in "$test_dir"/errors/*.vn; do
    [ -e "$source" ] || continue
    expected="${source%.vn}.error"
    if [ ! -f "$expected" ]; then
        report_failure "$(basename "$source") (missing .error file)"
        continue
    fi
    run_error_test "$source" "$expected"
done

echo
echo "Passed: $passed  Failed: $failed"
[ "$failed" -eq 0 ]
