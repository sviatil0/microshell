#!/usr/bin/env bash
# tests/integration.sh - end-to-end exercises: redirections, builtins,
# multi-stage pipelines, exit-status propagation.

set -u
MSH=${MSH:-./microshell}
TMP=$(mktemp -d -t microshell.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

if [[ ! -x "$MSH" ]]; then
  echo "integration.sh: $MSH not found" >&2
  exit 2
fi

fail=0
total=0

assert_eq() {
  total=$((total + 1))
  local desc="$1" got="$2" want="$3"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL: $desc"
    echo "  expected: $(printf '%q' "$want")"
    echo "  got:      $(printf '%q' "$got")"
    fail=$((fail + 1))
  else
    echo "ok: $desc"
  fi
}

# 1. > redirect creates a file with expected content
out=$TMP/r1
printf 'echo hi > %s\n' "$out" | "$MSH" >/dev/null 2>&1
assert_eq "redirect-create" "$(cat "$out")" "hi"

# 2. >> append
printf 'echo a > %s\necho b >> %s\n' "$out" "$out" | "$MSH" >/dev/null 2>&1
assert_eq "redirect-append" "$(cat "$out")" "$(printf 'a\nb')"

# 3. < input redirect
printf 'one\ntwo\nthree\n' > "$TMP/in"
got=$(printf 'wc -l < %s\n' "$TMP/in" | "$MSH" 2>/dev/null | tr -d ' ')
assert_eq "redirect-input" "$got" "3"

# 4. 2> stderr redirect
err=$TMP/r2err
printf 'ls /no_such_path_xyz 2> %s\n' "$err" | "$MSH" >/dev/null 2>&1
# We just care that the file is non-empty
nonempty=0
[[ -s "$err" ]] && nonempty=1
assert_eq "stderr-redirect-nonempty" "$nonempty" "1"

# 5. Exit-status propagation through $?
got=$(printf 'false\necho $?\n' | "$MSH" 2>/dev/null)
assert_eq "exit-status-propagate" "$got" "1"

# 6. exit with explicit code
got=$(printf 'exit 7\n' | "$MSH" 2>/dev/null; echo $?)
assert_eq "exit-code-7" "$got" "7"

# 7. cd + pwd
got=$(printf 'cd /\npwd\n' | "$MSH" 2>/dev/null)
assert_eq "cd-then-pwd" "$got" "/"

# 8. multi-stage pipe with awk-style filter
got=$(printf 'echo -e "a\\nb\\nc" | cat | wc -l | tr -d " "\n' | "$MSH" 2>/dev/null)
# Note: echo -e behavior varies; just sanity-check it produced a digit
[[ "$got" =~ ^[0-9]+$ ]] && ok=1 || ok=0
assert_eq "pipe-three-stages-numeric" "$ok" "1"

# 9. env builtin emits at least PATH=
got=$(printf 'env\n' | "$MSH" 2>/dev/null | grep -c '^PATH=' || true)
[[ "$got" -ge 1 ]] && ok=1 || ok=0
assert_eq "env-shows-PATH" "$ok" "1"

# 10. export then read back
got=$(printf 'export MSH_TEST=42\necho $MSH_TEST\n' | "$MSH" 2>/dev/null)
assert_eq "export-then-expand" "$got" "42"

# 11. quoted with embedded spaces survives pipe
got=$(printf 'echo "a  b   c" | cat\n' | "$MSH" 2>/dev/null)
assert_eq "quoted-spaces-survive" "$got" "a  b   c"

# 12. Background job: launch a quick true & and check exit
out=$(printf 'true &\necho done\n' | "$MSH" 2>/dev/null)
assert_eq "background-echo-runs" "$out" "done"

# 13. kill accepts a symbolic signal name (-SIGKILL), not just a number
err=$(printf 'sleep 5 &\nkill -SIGKILL %%1\n' | "$MSH" 2>&1 >/dev/null)
got=$(printf '%s' "$err" | grep -ci "invalid signal")
assert_eq "kill-symbolic-signal-accepted" "$got" "0"

# 14. kill rejects an unknown signal name cleanly
err=$(printf 'sleep 5 &\nkill -BOGUS %%1\n' | "$MSH" 2>&1 >/dev/null)
got=$(printf '%s' "$err" | grep -ci "invalid signal")
assert_eq "kill-bad-signal-rejected" "$got" "1"
pkill -f "sleep 5" 2>/dev/null || true

if (( fail > 0 )); then
  echo "integration.sh: $fail/$total cases failed" >&2
  exit 1
fi
echo "integration.sh: $total/$total ok"
