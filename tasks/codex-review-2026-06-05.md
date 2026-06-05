# Codex code review — 2026-06-05

Status: **open**. Findings from `codex exec` (gpt-5.5) review of `cli.py` + `trampoline.c`
ahead of the public release. None are release-blockers on their own, but #1–#5 are worth
fixing before tagging a PyPI version. Nothing here has been applied yet.

## Findings

### trampoline.c
1. **[medium] Double signal delivery.** `posix_spawn` leaves parent+child in the same
   foreground process group, so a terminal Ctrl-C hits the child directly *and* the
   wrapper's handler forwards it again (`trampoline.c:40`). Risk: double
   `KeyboardInterrupt` / interrupted cleanup. Fix: pick a process-group strategy, or
   don't re-forward terminal-delivered signals.
2. **[medium] Handler-install race.** A `SIGTERM`/`SIGHUP` arriving after `posix_spawn`
   but before `child_pid`/handlers are set kills only the wrapper, orphaning python
   (`trampoline.c:163`, `:171`). Fix: block forwarded signals around spawn + assignment,
   install handlers first, unblock once `child_pid` is valid.
6. **[low] `argc == 0`.** `child_argv[0] = python` is then overwritten by
   `child_argv[argc] = NULL` (`trampoline.c:144`). Pathological; normalize `argc < 1`.
7. **[low] Fixed `PATH_MAX` buffers** make very long install paths fail
   (`trampoline.c:59`, `:101`). Use dynamic alloc for `_NSGetExecutablePath` /
   `realpath(NULL)` / path joins.

### cli.py
3. **[medium] mtime-based build cache.** `--rebuild` can still reuse an old trampoline
   if the cached binary is newer than the freshly-installed `trampoline.c`
   (`cli.py:84`). Key the cache by source content hash + compiler flags/toolchain.
4. **[medium] Non-atomic install.** Rebuild writes the live binary before signing
   succeeds (`cli.py:142`–`:150`); a failed copy/sign leaves an unsigned/partial exe,
   and concurrent launches can observe it. Fix: build/sign/verify a temp file in `bin/`,
   then `os.replace`.
5. **[medium] Unverified cache restore.** A corrupt / wrong-identifier / wrong-arch file
   at `signed/<identifier>` is copied and chmodded without validation; `_cdhash`
   returning `"?"` doesn't fail the wrap (`cli.py:100`, `:139`). Fix: `codesign
   --verify`, parse `Identifier=`, handle arch / universal binaries.
8. **[low/hardening] PATH-resolved tools.** `cc` and `codesign` come from `$PATH`
   (`cli.py:88`, `:145`). For a security-sensitive launcher prefer `/usr/bin/codesign`
   and `/usr/bin/xcrun --find cc` (or an explicit trusted `CC`).

## Release-framing notes (also tracked in release-public.md)
- README/CLI still describe Automation/EventKit behavior as if verified
  (`README.md` grant section, `cli.py:160`) — keep conditional until the real macOS
  checks in `release-public.md` items 1–2 are done.
- Branding still `ai.mxs.tcc.*` (`cli.py:75`) — looks personal on PyPI/GitHub; make the
  identifier prefix configurable (release-public.md item c).
