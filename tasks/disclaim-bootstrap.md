# Task: disclaim-self bootstrap so the trampoline owns TCC under any launcher

Status: **implemented, pending real-launcher verification** — created 2026-06-05.

## Why (problem this fixes)

The release verification (`tasks/release-public.md` item 1) only proved the design under
**launchd** (ppid 1). Any launcher that spawns us *without* disclaiming TCC
responsibility — a GUI app, a terminal — leaves us inheriting the launcher's responsible
root instead of our own, so the stable signed identity is moot. This was unverified and
likely broken for the non-launchd case.

Codex review (session 019e9867, 2026-06-05) corrected the load-bearing assumption:

- macOS responsibility is process state (`struct proc.p_responsible_pid`). At
  `posix_spawn`, XNU **copies the parent's already-resolved responsible PID into the
  child** (`proc_set_responsible_pid(child, parent->p_responsible_pid)`). The child
  inherits the parent's *responsible root*, NOT the parent's own PID.
- So "spawn python and stay the parent" does **not** make the trampoline the identity.
  It works under launchd only because launchd/xpcproxy arrange for the trampoline to be
  *self-responsible*; python then inherits that.
- `execve` is transparent (same PID, state preserved). `posix_spawn` is where the
  copy-or-disclaim decision happens.
- Under a non-disclaiming GUI-app parent: `app → trampoline → python` attributes
  python's TCC access to the **app**, not the trampoline.

## The mechanism (disclaim-self bootstrap)

`responsibility_spawnattrs_setdisclaim(attr, true)` is private SPI (the same one LLDB
uses so the *debuggee*, not the debugger, owns TCC prompts). Disclaim makes the **spawned
child** its own responsible root. We must NOT disclaim on the python spawn (that makes
*python* — the churning interpreter path — the identity). Instead, two trampolines:

```
launcher (GUI app / terminal / launchd) → A (trampoline, inherited launcher's root)
   └─ posix_spawn(B, disclaim=TRUE)         # disclaim on the FIRST spawn
        B (same signed binary) = self-responsible → identity = stable trampoline ✓
          └─ posix_spawn(python, disclaim=FALSE)   # plain spawn
               python inherits B's root = stable trampoline identity ✓
```

- Disclaim flag on **A→B**, not B→python. B→python is an ordinary spawn; identity is
  inherited automatically because B is self-responsible with the stable signed image.
- A and B are the **same binary**. Env guard `TCC_VENV_DISCLAIMED=1` (set by A) tells B
  to skip the bootstrap and go straight to python.
- A stays alive as a thin supervisor of B (wait + forward signals/exit). Result:
  two-level `A → B → python` supervision.
- Side benefit: fixes the **terminal** case too (previously attributed to the terminal).

## Degradation / safety

- SPI unavailable (`dlsym` NULL) → A skips the bootstrap, runs python directly =
  today's launchd-correct behavior.
- `TCC_VENV_NO_DISCLAIM=1` opt-out (old single-process path) for A/B attribution testing.
- Always-on otherwise: re-disclaiming under launchd is a harmless ~1ms no-op.

## Implemented

- [x] `spawn_and_supervise(path, argv, disclaim_fn, manage_terminal)` extracted; used for
      both A→B and B→python.
- [x] Phase A bootstrap (guard + opt-out + graceful SPI-missing degrade) in `trampoline.c`.
- [x] `$TCC_VENV_CHDIR` cwd handling (absolute path / project root / off).
- [x] Signed-bytes cache keyed by `<identifier>.<source-tag>` so a changed trampoline
      actually ships (one-time re-grant) instead of restoring stale bytes.
- [x] `-Wall -Wextra` clean; smoke-tested: A→B→python chain, exit codes, SIGTERM→143,
      opt-out single-layer, all chdir modes.

## Verify before trusting (do NOT assume)

1. **Does the real GUI-app launcher disclaim its children?** If yes, bootstrap is a no-op
   and the old design already worked; if no, the bootstrap is REQUIRED. Method (codex):
   ```
   sudo launchctl procinfo "$PID" | grep responsible          # while the process is alive
   log stream --debug --predicate 'subsystem == "com.apple.TCC"'
   # trigger one fresh TCC access, inspect the TCC.db `client` row:
   # trampoline path / python path / uv / launcher app?
   ```
   Compare `TCC_VENV_NO_DISCLAIM=1` (old path) vs default (bootstrap). Self-contained
   reproducer: `/tmp/tcc-procinfo-test.sh` (run with sudo).
2. Confirm disclaim needs **no special entitlement** for an ad-hoc-signed binary (LLDB
   precedent says fine; verify on target macOS).
3. Re-test the interactive terminal path through the two-level `A → B → python`.
4. Real-fleet shakeout per `tasks/release-public.md` validation strategy.

## Follow-on (separate)

- `tcc-venv ensure --identity <id>`: create venv + `uv sync` + build/sign trampoline +
  print the launch command. The "git clone → one command → paste config" path.
- `--identity` explicit flag (path-independent identifier) alongside the derived default.
- Optional uvx-friendly self-healing entry (sync up front, wrap, exec trampoline) —
  identity-correct via the bootstrap, but per-launch latency; offer alongside the direct
  absolute-binary command, don't replace it. Do NOT leave `uv run` in the runtime chain.
