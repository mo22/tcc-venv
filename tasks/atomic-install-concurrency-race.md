# `_atomic_install` races itself: fixed tmp filename + `finally: unlink`

Status: **fixed and verified in the worktree; NOT committed, NOT published.**
Moritz approved the full scope (unique staging + retry + fast path + the two extra
race sites) on 2026-07-31. Remaining: commit, then ask before `uv build` / PyPI
upload, then bump the pins in E below and broadcast.

Raised 2026-07-31 from the WebShell session that traced a lost boot to this bug.

## What shipped in the fix (all in `cli.py`; `trampoline.c` untouched)

- `_staged()` — one contextmanager giving every staging path a per-process
  `mkstemp` name, used at all three racing sites (`_atomic_install`,
  `_build_unsigned`, the signed-cache write).
- `_symlink()` — symlink-then-`os.replace`, so the shim is never absent.
- `_install_is_current()` — fast path: an already-correct install does no writes.
- Bounded retry (`INSTALL_ATTEMPTS = 4`) with desynchronising backoff; codesign
  failure became the retryable `_SignFailed` instead of an immediate `_die`.
- `tests/test_concurrency.py` — 6 stdlib-unittest regressions.

Two findings beyond the original report, both fixed: `_build_unsigned` staged into
the **machine-wide** cache (so unrelated projects race there — it was the most
frequent failure site on a cold cache), and `_symlink` was unlink-then-create,
leaving a window with no shim at all (1983 observed gaps under 4 writers).

## Verification actually performed

- Exact reported command, 6 concurrent `tcc-venv run` × 3 rounds: **0.2.1 = 14/18
  failed; 0.2.2 = 18/18 passed.**
- Suite seen RED before the fix (5 failures), green after.
- Mutation-tested to prove it is load-bearing: fixed tmp + retry = 1 failure, fixed
  tmp + `INSTALL_ATTEMPTS=1` = 3 failures, unique tmp + `INSTALL_ATTEMPTS=1` =
  green. So unique staging is the real fix and the retry is belt-and-braces.
- cdhash claim confirmed: source tag `30a706afa15c600b` identical at HEAD and in the
  worktree; the live granted binaries (`python-tcc-webshell` `af30e493…`,
  `python-tcc-fileindex-mcp` `e4bf631d…`) both still `verify: ok` under 0.2.2 code
  and are byte-identical to their cache entries, so the fast path no-ops on them.
  **No FDA/Automation re-grant.**

## The bug

`src/tcc_venv/cli.py:186` (in `_atomic_install`):

```python
tmp = installed.with_name(f".{installed.name}.tmp")
...
finally:
    tmp.unlink(missing_ok=True)
```

The staging filename is **fixed**, so two concurrent `tcc-venv run` invocations against
the same venv + `--identifier-prefix` stage into the *same* path and delete each other's
file. The loser dies with an uncaught exception, whichever step it happens to be in:

- restore-from-signed-cache path (`cli.py:248`) → `FileNotFoundError` at `os.replace(tmp, installed)`
- fresh build+sign path (`cli.py:251`) → `codesign ... : No such file or directory`

`_ensure_installed` calls `_atomic_install` on **every** `tcc-venv run`, not just the first,
so any overlapping pair of runs can hit it — this is not a first-use-only problem.

## Evidence

Reproduced on a throwaway venv, **5 of 5 concurrent pairs failed**, traceback identical to
production. Three concurrent cold runs failed 3/3 on the build+sign path.

Real-world hits, both on `/Users/mmoeller/workspace/webshell/backend/.venv` +
`--identifier-prefix de.mxs.webshell`, which has two callers that are both `RunAtLoad`
launchd daemons and therefore start in the same second at every boot:

- `webshell/backend/webshell-tcc` (service `control.webshell.webshell`)
- `webshell/mcp/run-mcp.sh` (service `control.claude-remote-mcp.webshell-agent-mcp`)

Occurrences: **2026-07-31 boot** — agent-mcp's `os.replace` landed at 13:51:07 and killed
the WebShell backend's concurrent install; launchd restarted it at 13:51:08 and WebShell
was unreachable until 13:52:17. Also **2026-07-18** in the agent-mcp log (the other side
lost that time). Window is ~150 ms on an idle machine, but stretches to seconds under a
boot herd at load 25-35, which is when it bites.

## Proposed fix

Two parts, both inside `_atomic_install`:

1. **Unique staging name** — `f".{installed.name}.{os.getpid()}.tmp"` (or `tempfile.mkstemp`
   in `installed.parent`, which also removes the guessable-path aspect). The `finally`
   unlink then only ever removes this process's own file. This alone closes the race.
2. **Bounded retry** (belt-and-braces, requested explicitly by Moritz as defence in depth):
   retry the stage→sign→verify→replace sequence a few times on `FileNotFoundError` /
   transient `codesign` failure before giving up. Rationale: the callers cannot serialize
   this themselves — see "Why the callers can't fix it" below.

Please also add a regression test that runs N concurrent installs against a temp venv and
asserts all N exit 0. Without one this silently regresses.

## Important: this needs NO TCC re-grant

`_source_tag()` (cli.py:124) hashes **only `trampoline.c` + CFLAGS**, not `cli.py`. A
Python-side change therefore leaves the source tag, the signed cache key, and the
trampoline cdhash untouched — existing Full Disk Access / Automation grants to
`python-tcc-mac-mcp`, `python-tcc-webshell`, `python-tcc-fileindex` etc. all survive.
**Verify this before releasing** (`tcc-venv status`, or compare `codesign -dvvv` CDHash
before/after) and say so explicitly in your report, because it is the whole reason this
fix is cheap.

## Why the callers can't fix it

A wrapper cannot serialize this with a lock: `tcc-venv run` does the install and then
`os.execve`s the long-running command in the same process, so a lock acquired by the
wrapper has no point at which it can be released — the exec'd daemon would hold it
forever, and there is no install-only mode that `run` will skip. Hence the fix belongs
here.

## Release / rollout

- Bump `pyproject.toml` version (0.2.1 → 0.2.2) and note the fix in the changelog/README.
- **Do not publish to PyPI without asking Moritz first** — propose, then wait.
- After release: `uv tool upgrade tcc-venv`, then the pins below get bumped.

## Callers pinned to 0.2.1 that must be bumped to 0.2.2 afterwards

Track these down and report them; do **not** edit other repos yourself, just list them:

- ~~`webshell/backend/webshell-tcc` (shebang)~~ — **no longer pinned**: reverted to
  `>=0.2.0` in webshell 5881a14, so it picks up 0.2.2 by itself. Verified.
- ~~`webshell/mcp/run-mcp.sh`~~ — same, `>=0.2.0`. Verified.
- `mcp/claude-remote-mcp/control.yaml:57` (`mac-mcp`) — `uvx tcc-venv==0.2.1`, the
  **only** remaining exact pin. Not ours to edit; broadcast instead.
- `mcp/fileindex-mcp/control.yaml` and `straiqr/fileindex-mcp/control.yaml` — checked:
  these are **one repo**, not two (`straiqr/fileindex-mcp` is a symlink; identical
  `pwd -P`, one shared identity `local.tcc-venv.fileindex-mcp.0287383c`, one installed
  service `control.fileindex.watch`). Both paths are the same unpinned
  `uvx tcc-venv run …`, so they need no edit and pick up 0.2.2 on their own. Not a
  second concurrent pair — only WebShell has two services on one venv.

The WebShell repo carries a follow-up task file to bump its two pins once 0.2.2 ships.
**Broadcast when 0.2.2 is published** so the waiting sessions can bump.

## Working agreement

Investigate, verify the cdhash claim, then **propose and wait for Moritz's confirmation
before implementing**. Do not auto-finish; stay open for follow-up.
