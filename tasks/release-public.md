# Task: make tcc-venv release-ready (GitHub + PyPI)

Status: **open** — created 2026-06-05. Tool is built and working locally on alphagit-track;
this task tracks the work to publish it publicly.

## Goal
Publish `tcc-venv` as a small open-source tool (GitHub repo + PyPI package) that gives
uv/venv Python apps a stable, codesigned macOS TCC identity so Full Disk Access /
Automation grants survive uv/python upgrades and show a per-project name in the
permission dialog.

## Current state (what already exists)
- Package at `~/workspace/tcc-venv` (`tcc-venv` CLI, `tcc_venv` package).
- `tcc-venv wrap <venv>` installs `<venv>/bin/python-tcc-<project>` (signed trampoline) +
  `<venv>/bin/python-tcc` symlink (→ that on macOS, → `python` on non-macOS).
- Verified locally: correct venv prefix via the shim, deterministic cdhash across
  re-wrap, `$VIRTUAL_ENV` mismatch refused, signal forwarding + exit-code propagation,
  wipe-then-rewrap restores identical identity (uv-sync-proof).
- Codex review fixes already applied: self-locate only (no `$VIRTUAL_ENV` preference),
  collision-resistant identifier `ai.mxs.tcc.<project>.<hash8-of-venv-realpath>`,
  signed-bytes cache copy-back, absolute invocation (no relative shebang for control).

## Blocking before any public release (verify, don't assume)
1. **Verify Automation/EventKit inheritance on real macOS.** We only proved Full Disk
   Access. Trigger a Reminders/Calendar/Mail (EventKit + AppleEvents) access through a
   launchd-launched `python-tcc-<project>` and confirm the grant attaches to it (not to
   the python child / osascript). Codex flagged these may key on the sending process.
2. **Verify the TCC dialog/Settings display name** actually shows `python-tcc-<project>`
   (not `python-tcc`, `python`, or the resolved path) for a bare ad-hoc CLI binary
   invoked via the symlink. If it disappoints, fall back to a minimal signed `.app`
   with `CFBundleName`/`CFBundleIdentifier`.
3. **Document the foundations honestly.** It relies on *undocumented* TCC
   responsible-process inheritance and ad-hoc cdhash determinism that can drift across
   macOS/codesign versions. README needs a clear "unofficial, may break on a future
   macOS" caveat + a tested-OS-version matrix.

## Release checklist
### a) GitHub
- [ ] Create public repo `mo22/tcc-venv` (decide org/user). `gh repo create`.
- [ ] Mirror from alphagit (`git@alpha.mxs.de:mmoeller/tcc-venv.git`) → add GitHub remote.
- [ ] CI: GitHub Actions on macOS runner — build the trampoline, run the behavioral
      tests (venv prefix, determinism, `$VIRTUAL_ENV` guard, signals, exit codes).
- [ ] Standard audit stack per global setup (osv-scanner, gitleaks) — light, few deps.

### b) Deploy to PyPI
- [x] Confirm the `tcc-venv` name is free on PyPI (it is, 2026-06-05). Only `tcc-venv`
      is published; `python-tcc` is the installed binary/shim name, never a package.
      License: MIT.
- [ ] Trusted-publishing (PyPI OIDC) via GitHub Actions on tag, or manual `uv build` +
      `twine`/`uv publish`.
- [ ] `pipx install tcc-venv` / `uvx tcc-venv` smoke test from the published artifact.
- [ ] Note: wheel ships `trampoline.c`; build happens client-side at `wrap` time (needs
      `cc`). Document the Xcode CLT requirement.

### c) Parameterize the branding
- [x] Make the identifier prefix configurable (flag / env / config) instead of the
      hardcoded `ai.mxs.tcc.`. Done 2026-06-05: `--identifier-prefix` flag +
      `$TCC_VENV_IDENTIFIER_PREFIX`, default `local.tcc-venv`.
- [ ] Make the shim/binary name prefix (`python-tcc`) overridable if needed.
- [x] Strip any other mxs-specific assumptions (identifier prefix was the only one).

### d) Docs & framing
- [ ] README: the problem (uv/python upgrade kills TCC grants), the mechanism, install,
      `wrap`, the uv-sync re-wrap story, the OS-version caveats.
- [ ] **Security framing:** explicit that it does NOT bypass TCC — the user still grants
      explicitly in System Settings; it only stabilizes the identity. Avoid reading as a
      TCC-evasion aid.
- [ ] Comparison / prior art note (codesign-for-TCC blog posts, why a venv-level tool).

### e) Nice-to-have
- [ ] `tcc-venv unwrap` (restore plain `python-tcc` → none / clean up).
- [ ] `tcc-venv doctor` — check FDA state where detectable, surface stale wraps after uv sync.
- [ ] Optional `.app` bundle mode for predictable dialog naming.
- [ ] A `uv sync` convenience wrapper or git hook that re-runs `wrap`.

## Validation strategy
Use the personal fleet as the real-world shakeout before publishing: mac-mcp,
fileindex-mcp, webshell (agent-mcp). Keep it on alphagit until items 1–3 above are
confirmed on the macOS versions in use.
