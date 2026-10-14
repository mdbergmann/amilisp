#!/usr/bin/env bash
#
# Auto-review gate (pre-commit).
#
# Reviews the STAGED diff with a headless `claude`. If the review is clean the
# commit proceeds untouched. If it finds problems and AUTO_FIX is on, a fix
# agent edits the affected files, the fixes are re-staged, and the commit
# proceeds WITH the fixes included.
#
# Design principles:
#   * Review + tests are MANDATORY. Fail-CLOSED: a reviewer error, timeout, or
#     missing tool BLOCKS the commit — it is never silently skipped. The only
#     bypass is an explicit `git commit --no-verify`.
#   * The staged diff is written to a file and the reviewer is told to READ it
#     (with Read+Grep+Glob tools — no shell), NOT piped on stdin. Headless `claude -p` stalls
#     when a large diff arrives on stdin and it must answer in a single shot;
#     reading the diff as a file lets the agent work through it reliably.
#   * Never sweep in changes the user didn't stage (partial-staging guard).
#   * The INDEX the user staged is what gets committed.  The reviewer used to
#     run with Bash and once did `git stash` / `git stash pop` to build a
#     baseline: stash resets the index, pop restores the files UNSTAGED, and git
#     re-reads the index after this hook — so the commit went in EMPTY with the
#     changes left in the working tree; on a later run it went as far as
#     `git commit`, moving HEAD.  The reviewer has no shell now, and the staged
#     tree + HEAD are snapshotted before any agent runs: a moved index is
#     restored with `git read-tree`, a moved HEAD blocks the commit (index guard).
#
# Override behaviour via env vars (see scripts/review/README.md):
#   CLAUDE_AUTO_REVIEW=0   disable entirely (or use `git commit --no-verify`)
#   CLAUDE_AUTO_FIX=0      review + block on issues, but don't auto-fix
#   CLAUDE_REVIEW_MODEL    default: sonnet
#   CLAUDE_FIX_MODEL       default: sonnet
#   CLAUDE_REVIEW_TIMEOUT  default: 5400  (seconds = 90 min, if `timeout`/`gtimeout`
#                          exists; the agentic Read+grep review and the fix agent
#                          can each take many minutes, and a large multi-file diff
#                          — e.g. a 40 KB+ changeset — needs the extra headroom)
#
# Note: --max-budget-usd is intentionally NOT passed. It only caps pay-per-token
# API-call spend (and only with --print); it does nothing for a subscription
# (claude.ai) login, which is metered by plan rate limits, not dollars.

ENABLED="${CLAUDE_AUTO_REVIEW:-1}"
[ "$ENABLED" = "0" ] && exit 0

REVIEW_MODEL="${CLAUDE_REVIEW_MODEL:-sonnet}"
FIX_MODEL="${CLAUDE_FIX_MODEL:-sonnet}"
AUTO_FIX="${CLAUDE_AUTO_FIX:-1}"
REVIEW_TIMEOUT="${CLAUDE_REVIEW_TIMEOUT:-5400}"
RUN_TESTS="${CLAUDE_RUN_TESTS:-1}"          # stage 2: run the fast test tier
TEST_TARGET="${CLAUDE_TEST_TARGET:-test}"  # set to 'test-plus' to include sento
TEST_TIMEOUT="${CLAUDE_TEST_TIMEOUT:-600}"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0
cd "$ROOT" || exit 0

# Defensive: skip while a merge/rebase/cherry-pick is in progress.
GITDIR="$(git rev-parse --git-dir)"
if [ -d "$GITDIR/rebase-merge" ] || [ -d "$GITDIR/rebase-apply" ] || \
   [ -f "$GITDIR/MERGE_HEAD" ] || [ -f "$GITDIR/CHERRY_PICK_HEAD" ]; then
  exit 0
fi

CLAUDE_BIN="$(command -v claude || true)"
if [ -z "$CLAUDE_BIN" ]; then
  echo "[auto-review] 'claude' not on PATH — cannot run the mandatory review. COMMIT BLOCKED." >&2
  echo "[auto-review] Install 'claude', or bypass deliberately with 'git commit --no-verify'." >&2
  exit 1
fi

TIMEOUT_BIN="$(command -v timeout || command -v gtimeout || true)"
maybe_timeout() { # maybe_timeout <secs> <cmd...>
  if [ -n "$TIMEOUT_BIN" ]; then "$TIMEOUT_BIN" "$@"; else shift; "$@"; fi
}

# Index guard.  INDEX_TREE is the tree object of the staged index, taken before
# any agent or build runs.  index_guard <stage> compares the live index against
# it and, if an agent's `git stash`/`reset`/`add` moved it, puts the snapshot
# back so the commit carries exactly what was staged (see design principles).
# Fail-CLOSED if the restore itself fails: an unknown index must not be
# committed.
INDEX_TREE=""
HEAD_SHA="$(git rev-parse -q --verify HEAD 2>/dev/null || echo root)"
snapshot_index() { INDEX_TREE="$(git write-tree)"; }
index_guard() { # index_guard <stage-description>
  # HEAD moved (an agent committed/reset/checked out)?  git would fail the
  # real commit with "cannot lock ref 'HEAD'" anyway — say why, and block.
  head_now="$(git rev-parse -q --verify HEAD 2>/dev/null || echo root)"
  if [ "$head_now" != "$HEAD_SHA" ]; then
    echo "[auto-review] HEAD moved during $1 ($HEAD_SHA -> $head_now): an agent ran git commit/reset/checkout — COMMIT BLOCKED. Inspect 'git log', put the branch back, and commit again." >&2
    [ -n "$LOG" ] && printf '_Index guard: HEAD moved during %s (%s -> %s) — COMMIT BLOCKED._\n\n' "$1" "$HEAD_SHA" "$head_now" >> "$LOG"
    exit 1
  fi
  now="$(git write-tree)"
  [ "$now" = "$INDEX_TREE" ] && return 0
  echo "[auto-review] WARNING: the index changed during $1 (an agent ran git stash/reset/add?) — restoring the staged snapshot $INDEX_TREE." >&2
  if git read-tree "$INDEX_TREE" && [ "$(git write-tree)" = "$INDEX_TREE" ]; then
    [ -n "$LOG" ] && printf '_Index guard: the index was altered during %s and restored from the staged snapshot._\n\n' "$1" >> "$LOG"
    return 0
  fi
  echo "[auto-review] index guard: could not restore the staged snapshot — COMMIT BLOCKED. Re-stage your changes and commit again." >&2
  [ -n "$LOG" ] && printf '_Index guard: could not restore the staged snapshot during %s — COMMIT BLOCKED._\n\n' "$1" >> "$LOG"
  exit 1
}

# Stage 2: build + run the fast test tier on the resulting tree. Fail-CLOSED on a
# real test/compile failure (blocks the commit), but fail-OPEN if `make` is absent.
# Returns non-zero only when tests actually fail.
run_tests_or_abort() {
  [ "$RUN_TESTS" = "1" ] || return 0
  if ! command -v make >/dev/null 2>&1; then
    echo "[auto-review] 'make' not on PATH — cannot run the mandatory tests. COMMIT BLOCKED." >&2
    echo "[auto-review] Install 'make', set CLAUDE_RUN_TESTS=0, or 'git commit --no-verify'." >&2
    [ -n "$LOG" ] && printf '### Tests — BLOCKED (make not on PATH)\n\n' >> "$LOG"
    return 1
  fi
  TESTLOG=".reviews/last-test.log"
  echo "[auto-review] running 'make $TEST_TARGET' (set CLAUDE_RUN_TESTS=0 to skip)..." >&2
  TEST_START_TS="$(date '+%Y-%m-%d %H:%M:%S')"
  TEST_START_EPOCH="$(date '+%s')"
  if maybe_timeout "$TEST_TIMEOUT" make --no-print-directory "$TEST_TARGET" > "$TESTLOG" 2>&1; then
    TEST_RC=0
  else
    TEST_RC=1
  fi
  TEST_END_TS="$(date '+%Y-%m-%d %H:%M:%S')"
  TEST_DURATION=$(( $(date '+%s') - TEST_START_EPOCH ))
  if [ -n "$LOG" ]; then
    {
      printf '### Tests (`make %s`) — %s\n\n' "$TEST_TARGET" \
        "$([ $TEST_RC -eq 0 ] && echo PASSED || echo FAILED)"
      printf -- '- Started: %s\n' "$TEST_START_TS"
      printf -- '- Ended:   %s\n' "$TEST_END_TS"
      printf -- '- Duration: %ss\n\n' "$TEST_DURATION"
    } >> "$LOG"
  fi
  if [ $TEST_RC -eq 0 ]; then
    echo "[auto-review] tests passed ($TEST_TARGET) in ${TEST_DURATION}s." >&2
    return 0
  fi
  echo "[auto-review] TESTS FAILED ($TEST_TARGET) after ${TEST_DURATION}s — commit aborted. Full output: $TESTLOG" >&2
  echo "[auto-review] ----- last 30 lines -----" >&2
  tail -n 30 "$TESTLOG" >&2
  echo "[auto-review] -------------------------" >&2
  return 1
}

# Files staged for this commit (added/copied/modified/renamed).
STAGED_FILES="$(git diff --cached --name-only --diff-filter=ACMR)"
[ -z "$STAGED_FILES" ] && exit 0

mkdir -p .reviews
# Prune review logs older than 30 days so .reviews doesn't grow unbounded.
find .reviews -maxdepth 1 -name 'log-*.md' -type f -mtime +30 -delete 2>/dev/null

# Write the staged diff to a file the reviewer will READ (not pipe on stdin).
DIFF_FILE="$ROOT/.reviews/staged.diff"
git diff --cached --no-color > "$DIFF_FILE"
[ -s "$DIFF_FILE" ] || exit 0
snapshot_index

TS="$(date '+%Y-%m-%d %H:%M:%S')"
PARENT="$(git rev-parse --short HEAD 2>/dev/null || echo root)"

# One fresh, timestamped log file per review (not an append-only history).
LOG=".reviews/log-$(date '+%Y%m%d-%H%M%S').md"
printf '# Auto-review log — %s (parent %s)\n\n' "$TS" "$PARENT" > "$LOG"

REVIEW_PROMPT='You are reviewing a git diff for a commit about to be made to AmiLisp, a Common Lisp environment for AmigaOS written in C89/C99. The staged diff to review is in the file '"$DIFF_FILE"' — read that file first.

Enforce these project rules (from CLAUDE.md):
- GC safety: any CL_Obj held across an allocating call (cl_alloc/cl_cons/cl_make_*/cl_vm_apply etc.) must be CL_GC_PROTECT-ed; watch for list-building loops.
- 32-bit-clean heap structs: no size_t or pointer-sized fields; use uint32_t/int32_t explicitly.
- C89/C99 only: no C11+ features.
- Common Lisp behaviour must conform to the HyperSpec.
- Every feature/bugfix needs tests; every bug fix needs a regression test.

Output format, STRICTLY:
- The FIRST line must be exactly "STATUS: CLEAN" or "STATUS: ISSUES".
- If ISSUES, follow with a markdown list, one item per finding:
  "- [HIGH|MED|LOW] path:line - problem - suggested fix"
- Report ONLY substantive problems (bugs, GC-safety, memory/32-bit, C89/C99, HyperSpec deviations, missing/incorrect tests, security). No style nits, no speculation.
- Read the diff file and any surrounding source files you need for context (Read tool); use Grep/Glob to search the tree and verify claims. You have no shell: do not try to build, run tests, or run git — you are inside a pre-commit hook and the hook runs the tests itself after you. Do NOT edit anything.'

# No stdin: the diff is read from $DIFF_FILE. Redirect </dev/null so headless
# claude doesn't wait on (or block reading) an empty stdin.  Read+Grep+Glob
# only: the agent can read the diff/sources and search the tree, but has NO
# shell — a reviewer with Bash ran `git stash`/`pop` (emptying the commit) and,
# on a later run, `git commit` (moving HEAD so the real commit could not lock
# the ref).  Verification by grep does not need a shell.
REVIEW_OUT="$(maybe_timeout "$REVIEW_TIMEOUT" "$CLAUDE_BIN" -p "$REVIEW_PROMPT" \
  --model "$REVIEW_MODEL" \
  --tools "Read,Grep,Glob" \
  --permission-mode bypassPermissions \
  --output-format text </dev/null 2>/dev/null)"
RC=$?
index_guard "the review"

if [ $RC -ne 0 ] || [ -z "$REVIEW_OUT" ]; then
  echo "[auto-review] reviewer error/timeout (rc=$RC) — COMMIT BLOCKED. The mandatory review did not complete." >&2
  echo "[auto-review] Retry, raise CLAUDE_REVIEW_TIMEOUT (now ${REVIEW_TIMEOUT}s), or bypass with 'git commit --no-verify'." >&2
  {
    printf '## %s — parent %s (staged)\n\n' "$TS" "$PARENT"
    printf '_Reviewer error/timeout (rc=%s) — COMMIT BLOCKED (review did not complete)._\n\n' "$RC"
  } >> "$LOG"
  exit 1
fi

# The reviewer is INSTRUCTED to put "STATUS: CLEAN|ISSUES" on the first
# line, but in practice it sometimes leads with a sentence of prose and
# the STATUS line follows.  Grep the first explicit STATUS: line anywhere
# in the output; fall back to line 1 only if none is present.  Without
# this, a CLEAN review whose first line is prose is mis-parsed as ISSUES
# and the commit is blocked despite a recorded clean verdict.
STATUS_LINE="$(printf '%s\n' "$REVIEW_OUT" | grep -m1 '^STATUS:' || printf '%s\n' "$REVIEW_OUT" | head -n1)"

{
  printf '## %s — parent %s (staged)\n\n' "$TS" "$PARENT"
  printf 'Files: %s\n\n' "$(echo "$STAGED_FILES" | tr '\n' ' ')"
  printf '%s\n\n' "$REVIEW_OUT"
} >> "$LOG"

case "$STATUS_LINE" in
  *CLEAN*)
    echo "[auto-review] review clean." >&2
    run_tests_or_abort; TESTS_RC=$?
    index_guard "the tests"
    [ $TESTS_RC -eq 0 ] || exit 1
    echo "[auto-review] commit proceeding." >&2
    exit 0
    ;;
esac

echo "[auto-review] issues found (logged to $LOG)." >&2

if [ "$AUTO_FIX" != "1" ]; then
  echo "[auto-review] AUTO_FIX disabled — aborting commit. Fix the findings, or 'git commit --no-verify' to bypass." >&2
  exit 1
fi

# Partial-staging guard: a file that is BOTH staged and has unstaged edits cannot
# be auto-restaged without sweeping in the unstaged hunks. Detect and bail safely.
UNSTAGED_TRACKED="$(git diff --name-only)"
PARTIAL=""
while IFS= read -r f; do
  [ -z "$f" ] && continue
  if printf '%s\n' "$UNSTAGED_TRACKED" | grep -Fxq -- "$f"; then
    PARTIAL="$PARTIAL $f"
  fi
done <<EOF
$STAGED_FILES
EOF

echo "[auto-review] applying automatic fixes..." >&2

FIX_PROMPT='Read '"$LOG"' and address the findings in its single review section. For each finding not already marked RESOLVED or DISMISSED:
- Edit the code to fix it. ONLY modify files listed in that entry'\''s "Files:" line — do not touch any other file.
- Make minimal, correct fixes. Follow CLAUDE.md: GC_PROTECT CL_Obj across allocations, keep heap structs 32-bit-clean, C89/C99 only, conform to the HyperSpec, and add/adjust tests when the finding calls for it (only within the listed files).
- Then append under that finding in the log: "  - RESOLVED: <what you changed>". If a finding is a false positive, append "  - DISMISSED: <why>" and change no code for it.
- Do NOT run git and do NOT commit. Only edit files and update the log.'

maybe_timeout "$REVIEW_TIMEOUT" "$CLAUDE_BIN" -p "$FIX_PROMPT" \
  --model "$FIX_MODEL" \
  --permission-mode acceptEdits \
  --allowedTools "Read,Grep,Glob,Edit,Write" \
  --output-format text >/dev/null 2>&1
FRC=$?
index_guard "the fix agent"

if [ $FRC -ne 0 ]; then
  echo "[auto-review] fix agent error/timeout (rc=$FRC) — aborting commit. Any fixes are in your working tree; re-stage and commit, or use --no-verify." >&2
  exit 1
fi

if [ -n "$PARTIAL" ]; then
  echo "[auto-review] partially-staged files (staged + unstaged edits):$PARTIAL" >&2
  echo "[auto-review] NOT auto-staging — would sweep in unstaged changes. Fixes are in your working tree." >&2
  echo "[auto-review] review, 'git add' what you want, and commit again (or --no-verify)." >&2
  exit 1
fi

# Re-stage exactly the originally-staged files (now carrying the fixes).
echo "$STAGED_FILES" | while IFS= read -r f; do
  [ -n "$f" ] && git add -- "$f"
done

echo "[auto-review] fixes applied and re-staged." >&2
snapshot_index   # the re-staged fixes are now the intended index
run_tests_or_abort; TESTS_RC=$?
index_guard "the tests"
[ $TESTS_RC -eq 0 ] || exit 1
echo "[auto-review] commit proceeding WITH fixes (see $LOG; 'git show HEAD' after)." >&2
exit 0
