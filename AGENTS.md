# AGENTS.md — tcc-venv

Project-level instructions and design notes for agents working in this repo.
Tool-agnostic; `CLAUDE.md` is a symlink to this file.

## What this is

`tcc-venv` installs a stable, codesigned **macOS TCC launcher** into a uv/venv so
Full Disk Access / Automation grants survive `uv sync` / Python upgrades and show a
per-project name in the permission dialog.

The core problem: macOS TCC keys privacy grants on the *binary identity* (path +
code-signing identity / cdhash) of the responsible process. A uv/venv `python` lives
at a version-pinned, churning path (`.../cpython-3.12.x-macos.../bin/python3.12`),
so every interpreter upgrade looks like a *different* app to TCC and silently drops
the grant. `tcc-venv` interposes a tiny signed C launcher with a **stable identity**
that the user grants once.

## Layout

```
src/tcc_venv/
  cli.py          # the `tcc-venv` CLI: wrap / status. build + codesign + cache logic.
  trampoline.c    # the signed launcher. self-locates its venv, spawns python, forwards signals.
  __init__.py
pyproject.toml    # hatchling; force-includes trampoline.c into the wheel.
```

## How it works

`tcc-venv wrap <venv>` (macOS):

1. Resolve the venv (`pyvenv.cfg` must exist).
2. Derive a friendly name (`pyvenv.cfg` `prompt`, else parent dir) and a
   collision-resistant identifier `<prefix>.<name>.<sha256(realpath)[:8]>`, where
   `<prefix>` defaults to `local.tcc-venv` and is overridable via
   `--identifier-prefix` / `$TCC_VENV_IDENTIFIER_PREFIX`.
3. Compile `trampoline.c` once per arch → cached unsigned binary.
4. Install it as `<venv>/bin/python-tcc-<name>`, ad-hoc codesign with
   `--identifier <identifier>`, and cache the **signed bytes** keyed by identifier.
5. Symlink `<venv>/bin/python-tcc -> python-tcc-<name>` (the uniform name used in
   shebangs / process control).

On re-wrap (after `uv sync` blows the binary away), the **signed bytes are copied
back** from cache, so the cdhash — and therefore the TCC grant — is identical by
construction, immune to codesign-version drift.

The trampoline:
- self-locates its venv from its own executable path (`_NSGetExecutablePath` +
  `realpath`), **never** trusts `$VIRTUAL_ENV` to pick the interpreter — it only
  *refuses to run* if `$VIRTUAL_ENV` disagrees with the self-located venv.
- `posix_spawn`s `<venv>/bin/python` (falls back to `python3`) and stays the live
  parent (must NOT exec into python, or the child becomes the responsible process
  and loses the grant).
- runs the child in its **own process group** and, when interactive, hands it the
  controlling terminal (`tcsetpgrp`) so terminal-generated signals reach the child
  once — not the wrapper-then-child twice. Forwarded signals are blocked across
  the spawn, and the child is started with a default mask + dispositions
  (`POSIX_SPAWN_SETSIGMASK | SETSIGDEF`) so it actually receives them.
- forwards SIGINT/TERM/HUP/QUIT/USR1/USR2 (directed at the wrapper) to the child's
  process group, propagates exit status (`128 + signo` on signal death).

On non-macOS the installer just symlinks `python-tcc -> python` (pure passthrough;
the C file still compiles to a plain `execv`).

## Design invariants (don't regress these)

- **Bytes are project-independent.** Identity comes from `codesign --identifier` +
  the on-disk filename, not from the compiled bytes. One cached build → every venv.
- **Determinism over re-signing.** Always prefer copy-back of cached signed bytes;
  only `--rebuild` mints a new cdhash (and forces re-granting FDA).
- **Self-locate, never `$VIRTUAL_ENV`-redirect.** A leaked `$VIRTUAL_ENV` must never
  make this binary run another project's interpreter under this identity.
- **Stay the parent.** Spawn + wait, don't exec, so TCC inheritance holds.

## Status / caveats

This relies on **undocumented** TCC responsible-process inheritance and ad-hoc
cdhash determinism that *can* drift across macOS / codesign versions. It does **not**
bypass TCC — the user still grants explicitly in System Settings; it only stabilizes
the identity. Treat as unofficial; verify on the macOS versions you ship to.

Open release work is tracked in `tasks/release-public.md`.

## Conventions

- Format Python with `uvx ruff format` / `uvx ruff check --fix`.
- No runtime deps (stdlib only). Keep it that way — this is a tiny tool.
- The trampoline must stay warning-clean under `-Wall -Wextra`.
