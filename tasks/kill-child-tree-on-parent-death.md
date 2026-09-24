# Task: kill the child process tree when the trampoline's parent dies

Status: **implemented, verified — awaiting release** (code done 2026-09-24, dispatched
from agent-setup session dc961425). The trampoline change is committed; the FDA
re-grant it forces means **release/tag/publish and upgrading the installed `uvx
tcc-venv` still need Moritz's go-ahead** (Constraints below), so those steps are NOT
done.

## Goal

When the trampoline's parent process dies (for any reason, including SIGKILL), the
trampoline must tear down its whole child process tree and exit, instead of living on
reparented to launchd (PPID 1) with the payload still running.

## Why (incident, 2026-09-24)

The fileindex-mcp `watch` daemon runs under control/launchd as
`/bin/sh -c "uvx tcc-venv run --cd-to-project uv run --frozen fileindex watch …"`.
A stop left the whole chain below `uvx` running as orphans for days. The orphan held
fileindex's instance lock, so every launchd restart exited with "Lock held by another
process", and the stale process grew to a 103 GB footprint.

Live process tree of that job (PGID column shows the problem):

```
  PID  PPID  PGID  COMMAND
34374     1 34374  uv tool uvx tcc-venv run --cd-to-project uv run --frozen fileindex watch …   <- launchd job main pid
34383 34374 34374  .venv/bin/python-tcc-fileindex-mcp /opt/homebrew/bin/uv run …              <- trampoline (bootstrap layer)
34407 34383 34407  .venv/bin/python-tcc-fileindex-mcp /opt/homebrew/bin/uv run …              <- trampoline (disclaimed layer), NEW group
34409 34407 34409  uv run --frozen fileindex watch …                                           <- NEW group (trampoline.c:224-226)
34413 34409 34409  Python … fileindex watch
```

### Measured mechanism

1. launchd's stop (`launchctl bootout`, `control stop/restart`) sends SIGTERM to the
   job's main pid (`uvx`). `uvx` forwards it, and the trampolines forward it via
   `forward_signal` — that path works (verified: plain SIGTERM to `uvx` shuts the
   whole chain down cleanly).
2. If the payload has not exited within the job's `ExitTimeOut` (5 s for control
   jobs), launchd **SIGKILLs only the main pid** (`uvx`). It does NOT kill the process
   group. The trampoline (still in uvx's group) survives, is reparented to PID 1, and
   keeps everything below it alive.
3. SIGKILL cannot be forwarded, and nothing in the trampoline notices that its parent
   died. Each trampoline layer also spawns its child with `POSIX_SPAWN_SETPGROUP` /
   pgroup 0, so even a group kill from above would only reach one layer.

The trigger on the fileindex side (SIGTERM ignored until the current sync pass ended)
is fixed in fileindex-mcp `f6cb54f`. This task is the generic safety net: any
tcc-venv daemon that is slow to exit, or whose launcher crashes, currently leaks its
tree.

### Reproduction (launchd probe, verified 2026-09-24)

Scratch LaunchAgent, payload ignores SIGTERM, `ExitTimeOut` 5, no `RunAtLoad`:

```xml
<key>Label</key><string>local.probe.tccvenv-orphan</string>
<key>ProgramArguments</key><array><string>/bin/sh</string><string>-c</string>
<string>uvx tcc-venv run --cd-to-project uv run --frozen python -c 'import signal,time; signal.signal(15, lambda *a: None); time.sleep(900)  # orphanprobe'</string></array>
<key>WorkingDirectory</key><string><any repo with a tcc-venv .venv></string>
<key>EnvironmentVariables</key><dict><key>PATH</key><string>/opt/homebrew/bin:$HOME/.local/bin:/usr/bin:/bin</string></dict>
<key>ExitTimeOut</key><integer>5</integer>
```

`launchctl bootstrap gui/$UID <plist>; launchctl kickstart gui/$UID/<label>`, wait for
the payload, `launchctl bootout gui/$UID/<label>`, wait 8 s, then
`pgrep -fl orphanprobe`. Current result: outer trampoline with PPID 1 plus the inner
trampoline, `uv run` and python all still alive. Clean up with `kill -KILL` on the
listed pids (the payload ignores TERM).

For a test run from this repo, point the probe at a scratch venv wrapped with the
locally built tcc-venv (e.g. `uv run --project <this repo> tcc-venv run …`) rather
than the installed `uvx tcc-venv`, which is the released tool version.

## Requirements

1. Detect parent death promptly, including when the parent was SIGKILLed. On macOS,
   kqueue `EVFILT_PROC` / `NOTE_EXIT` on the parent pid is the obvious mechanism.
   Register it, then re-check `getppid()` against the original parent to close the
   race where the parent died before registration.
2. On parent death, kill the **whole** child process tree, not just one layer:
   - Both trampoline layers (bootstrap and disclaimed) run the same binary, so both
     get the watch. Pitfall: if the outer layer escalates to SIGKILL on the inner
     trampoline's group, the inner trampoline dies without forwarding, and the
     `uv run` group below it is orphaned again. The escalation has to reach every
     group in the tree, or each layer has to handle its own child group when its own
     parent dies.
   - Suggested order: SIGTERM to the child group, short grace period (a few
     seconds), then SIGKILL to the child group; then exit.
3. Keep existing behaviour intact: signal forwarding, exit-code propagation,
   interactive terminal handoff (`tcsetpgrp`), disclaim bootstrap.
4. Don't create a visible behaviour change for interactive use. A terminal closing
   already sends SIGHUP down, but check that the new watch doesn't fire spuriously.

## Constraints

- 🚨 **Changing `trampoline.c` changes the cdhash** (`_source_tag()` hashes
  `trampoline.c` + CFLAGS), so every wrapped venv needs a **Full Disk Access
  re-grant** once the new version is installed. Batch the change so this happens
  once, and name the re-grant in the release notes.
- Releasing, tagging, publishing or upgrading the installed `uv tool` (`tcc-venv`
  is used unpinned through `uvx` by fileindex-mcp's `control.yaml` and `run-mcp.py`)
  needs direct confirmation from Moritz. Stop before that step and report.

## Verification (definition of done)

1. The launchd probe above leaves **no** `orphanprobe` process within a few seconds
   after `bootout`, using the new trampoline.
2. The same probe with a payload that *does* honour SIGTERM still exits cleanly
   (no regression in forwarding / exit code).
3. `kill -KILL <parent>` of a trampoline started from a shell tears down the tree.
4. Interactive `tcc-venv run python` (REPL, Ctrl-C) behaves as before.
5. Existing tests pass (`tests/test_concurrency.py`); add a regression test for
   parent death if it can run without launchd (e.g. spawn trampoline under a
   throwaway parent, SIGKILL the parent, assert the grandchild is gone).

## Implementation (2026-09-24, session 019JyTQi)

`trampoline.c`: `spawn_and_supervise` now waits on a kqueue watching **both** the
child (`EVFILT_PROC`/`NOTE_EXIT`) and the parent pid, with a `getppid()` re-check +
`EV_RECEIPT` `ESRCH` handling to close the register-after-death race. Forwarded
signals still go through the existing handlers (they just interrupt `kevent`, which
resumes). On parent death, `parent_died_teardown()` runs, and it is **layer-aware**
(new `child_is_trampoline` arg):

- **leaf layer** (child = python / the `tcc-venv run` command): SIGTERM the child
  group → ~3 s grace (50 ms polls) → SIGKILL the child group → exit. Kills the real
  payload even when it ignores SIGTERM, and reaches same-group grandchildren.
- **bootstrap layer** (child = the disclaimed copy of the trampoline): SIGTERM +
  exit only. It must NOT SIGKILL the inner trampoline — that was the pitfall in
  §Requirements 2: the inner layer would die before tearing down the `uv run` group,
  re-orphaning it. Exiting lets the inner layer detect our death via its own parent
  watch and escalate on the group it actually parents.

Documented in `AGENTS.md` (trampoline bullet + a "never SIGKILL a trampoline child"
invariant).

## Measured (this repo, local build; NOT via released `uvx tcc-venv`)

Reproduction harness = the §Reproduction shape without launchd: a throwaway parent
(`launcher.py`) starts the chain, we SIGKILL it, then `pgrep -f <unique tag>`. Payload
ignores SIGTERM. All on a scratch venv wrapped with the locally-built trampoline.

1. **Def-of-done 1 (orphan probe).** Direct `python-tcc payload` (bootstrap+disclaimed
   +python) and the incident-faithful `tcc-venv run <python> payload fork` (extra
   process group below the disclaimed layer, like `uv run python`, plus a same-group
   grandchild): after SIGKILL of the parent, **no tagged process survived** (~4 s,
   the grace). The forked grandchild's recorded pid was confirmed gone.
   - **Load-bearing check:** the *same* probe against a venv wrapped with HEAD's
     pre-fix trampoline leaves the whole tree (both trampolines + payload + child)
     orphaned with PPID 1 — reproduces the incident. Fix ⇒ empty, pre-fix ⇒ orphans.
2. **Def-of-done 2 (no forwarding regression).** SIGTERM to the chain root with a
   SIGTERM-honouring payload: whole chain exits **rc=0 in 0.03 s** (no grace delay —
   the parent-death path stays dormant while the parent lives). Exit-code propagation
   (`sys.exit(42)` → 42) and signal-death propagation (self-SIGTERM → 143) intact.
3. **Def-of-done 3 (`kill -KILL <parent>`).** Covered by test 1 (parent SIGKILLed).
4. **Def-of-done 4 (interactive).** REPL under a real PTY: Ctrl-C raises
   `KeyboardInterrupt` without killing the REPL, session continues, `exit()` → rc 0.
   The parent watch does not fire spuriously (parent stays alive).
5. **Def-of-done 5 (tests).** `tests/test_concurrency.py` still green (6/6). Added
   `tests/test_parent_death.py` (2 tests, macOS-only, stdlib unittest): direct chain +
   `run`-with-forked-grandchild. Both pass on the fix and **both fail against the
   reverted pre-fix trampoline** (4 orphans each) — confirmed by swapping
   `trampoline.c` for `git show HEAD:` and re-running. Trampoline stays warning-clean
   under `-Wall -Wextra -O2`.

### Not done (needs Moritz) — the release

Changing `trampoline.c` busts `_source_tag()` ⇒ new cdhash ⇒ **one-time FDA re-grant
for every wrapped venv** once the new version is installed. Do not tag/publish or
`uv tool upgrade tcc-venv` (used unpinned via `uvx` by fileindex-mcp's `control.yaml`
/ `run-mcp.py`) without confirmation. Release notes must name the re-grant. The
launchd probe in §Reproduction should be re-run once against the *released* build as
the final sign-off (this session verified the local build only).
