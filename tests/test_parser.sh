#!/usr/bin/env bash
# tests/test_parser.sh - feed parser edge cases via stdin and verify output.
#
# Each case is a self-contained block: stdin script + expected stdout.
# We diff the captured stdout against the expectation. stderr is ignored
# unless explicitly asserted.

set -u
MSH=${MSH:-./microshell}

if [[ ! -x "$MSH" ]]; then
  echo "test_parser.sh: $MSH not found or not executable" >&2
  exit 2
fi

fail=0
total=0

run_case() {
  local desc="$1" script="$2" expected="$3"
  total=$((total + 1))
  local got
  got=$(printf '%s\n' "$script" | "$MSH" 2>/dev/null) || true
  if [[ "$got" != "$expected" ]]; then
    echo "FAIL: $desc"
    echo "  script:   $script"
    echo "  expected: $(printf '%q' "$expected")"
    echo "  got:      $(printf '%q' "$got")"
    fail=$((fail + 1))
  else
    echo "ok: $desc"
  fi
}

# --- basics ---
run_case "echo plain"          'echo hello'                'hello'
run_case "double quotes"       'echo "hello world"'        'hello world'
run_case "single quotes"       "echo 'a b c'"              'a b c'
run_case "multiple args"       'echo a b c'                'a b c'

# --- variable expansion ---
run_case "env var expand"      'export X=42
echo $X'                       $'42'
run_case "braced var"          'export X=42
echo ${X}done'                 $'42done'
run_case "last status zero"    'echo $?'                   '0'
run_case "no expand in singles" "echo 'l_\$X'"             'l_$X'

# --- comments ---
run_case "comment whole line"  '# just a comment
echo after'                    'after'
run_case "comment trailing"    'echo hi # tail'            'hi'

# --- pipes ---
run_case "two stage pipe"      'echo abc | tr a-z A-Z'     'ABC'
run_case "three stage pipe"    'echo abc | tr a-z A-Z | rev'  'CBA'

# --- escape ---
run_case "escaped space"       'echo hello\ world'         'hello world'
run_case "escaped dollar"      'echo \$X'                  '$X'

# --- empty/no-op ---
run_case "blank input"         ''                          ''
run_case "only comment"        '# nothing here'            ''

if (( fail > 0 )); then
  echo "test_parser.sh: $fail/$total cases failed" >&2
  exit 1
fi
echo "test_parser.sh: $total/$total ok"
