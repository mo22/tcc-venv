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
1. **[VERIFIED 2026-06-05 on mac-mcp]** Automation/EventKit inheritance works on real
   macOS. Under launchd (ppid 1, no terminal ancestor), `python-tcc-mac-mcp` reads
   GoogleDrive (kTCCServiceFileProviderDomain) AND Reminders (kTCCServiceReminders).
   TCC.db records BOTH grants against the binary path
   `.../.venv/bin/python-tcc-mac-mcp` = ALLOW — so EventKit attributes to the
   responsible signed parent, not the python child. (Calendar/Mail same class; will
   prompt on first use.) Tested macOS: Darwin 25.5.0.
2. **[VERIFIED 2026-06-05]** Display identity = the per-project binary. TCC.db stores
   the client as `python-tcc-<project>`'s full path, so the prompt/Settings show that
   name — no `.app` bundle needed. (Re-confirm the human-readable dialog string on a
   couple more macOS versions before claiming it broadly.)
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
- [x] Trusted-publishing (PyPI OIDC) via GitHub Actions on release. `.github/workflows/
      publish.yml` + repo `pypi` environment + PyPI pending publisher. **v0.1.0 published
      2026-06-05** (release tag v0.1.0 → workflow run 27017162063, green).
- [x] `uvx tcc-venv` smoke test from the published artifact — installs and runs `status`.
- [x] Wheel ships `trampoline.c`; build is client-side at `wrap` time (needs `cc`). Xcode
      CLT requirement documented in README ("Requirements").

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
