# tcc-venv

**Give a uv/venv Python app a stable, codesigned macOS TCC identity** — so Full Disk
Access / Automation grants survive `uv sync` and Python upgrades, and the permission
dialog shows a recognizable per-project name instead of `python3.12`.

> macOS only for the privacy benefit. On Linux/Windows it degrades to a no-op
> passthrough so the same launcher name works everywhere.

## The problem

macOS TCC (the privacy system behind *Full Disk Access*, *Automation*, *Calendar*,
etc.) grants permissions to a specific **binary identity** — its path plus its
code-signing identity. A Python interpreter created by `uv` lives at a churning,
version-pinned path:

```
~/.local/share/uv/python/cpython-3.12.7-macos-aarch64-none/bin/python3.12
```

Grant *that* binary Full Disk Access, then run `uv python upgrade` or let `uv sync`
pull a new patch release, and the path changes. To TCC it's now a **different app**:
the grant silently stops applying and your tool starts getting "Operation not
permitted" until you re-grant it by hand. The dialog also just says "python3.12",
giving the user no idea which project is asking.

`tcc-venv` fixes both by interposing a tiny **signed C launcher** with a stable
identity that you grant *once*:

```
<venv>/bin/python-tcc-<project>   # signed trampoline — the stable TCC identity
<venv>/bin/python-tcc             # -> python-tcc-<project> (uniform name for shebangs/control)
```

The launcher self-locates its venv, spawns `<venv>/bin/python` with your arguments,
forwards signals, and propagates the exit code — staying the parent process so TCC
attributes everything to *it*. To make that hold even when the launcher above it does
*not* hand it a TCC identity (a GUI app, a terminal — anything that isn't launchd), it
re-spawns a self-responsible copy of itself first (a "disclaim" bootstrap), so the
stable signed identity owns the grant regardless of who started it. Re-running `wrap`
after `uv sync` restores the **identical** signed bytes from a local cache, so the
cdhash — and the grant — is unchanged.

Set `TCC_VENV_CHDIR=1` to run from the project root (the venv's parent), or to a path
to `cd` there — handy when the launcher gives an unpredictable working directory.

### What it is *not*

It does **not** bypass or weaken TCC. You still grant access explicitly in System
Settings; this only stops the identity from moving out from under that grant. It
relies on *undocumented* TCC responsible-process inheritance and ad-hoc cdhash
determinism, both of which can change across macOS releases — treat it as unofficial
and verify on the macOS versions you ship to.

**Tested:** on macOS Darwin 25.5.0, both Full Disk Access and EventKit/Reminders
access through a launchd-launched (no terminal ancestor) `python-tcc-<project>`
attach the grant to that signed binary — TCC records it against the per-project
binary path, not the python child — confirming the responsible-process inheritance
this tool depends on. Re-confirm on the macOS versions you target.

## Requirements

- macOS with the **Xcode Command Line Tools** (`xcode-select --install`) — the
  trampoline is compiled with `cc` client-side at `wrap` time.
- Python ≥ 3.11.
- A venv with a `pyvenv.cfg` (uv, `python -m venv`, pdm-in-venv mode — all fine).

## Install

```bash
uvx tcc-venv wrap            # run without installing
# or
pipx install tcc-venv
# or
uv tool install tcc-venv
```

## Usage

Run `wrap` once per venv, then point your launchers / shebangs at `python-tcc`.

### With uv

```bash
cd my-project
uv sync                      # creates ./.venv
uvx tcc-venv wrap            # defaults to ./.venv
```

Because `uv sync` recreates the interpreter, re-run `wrap` after a sync — it restores
the same identity from cache, so you do **not** re-grant:

```bash
uv sync && uvx tcc-venv wrap
```

### With pdm

pdm can manage an in-project venv. Enable it, install, then wrap:

```bash
pdm config python.use_venv true
pdm install                  # creates ./.venv
uvx tcc-venv wrap .venv
```

### With a bare venv

```bash
python3 -m venv .venv
.venv/bin/pip install -e .
uvx tcc-venv wrap .venv
```

Tip: `python -m venv --prompt myapp .venv` sets the friendly name used in the
identity and the TCC dialog (otherwise the venv's parent directory name is used).

## Grant the permission (once)

After `wrap`, the binary path is printed. Add **that** binary to the relevant pane:

> System Settings → Privacy & Security → Full Disk Access → **+** → select
> `<venv>/bin/python-tcc-<project>`

Automation / Calendar / Reminders prompts appear on first use. From then on, run your
app through the shim:

```bash
.venv/bin/python-tcc my_app.py
# or in a shebang:  #!/path/to/.venv/bin/python-tcc
```

## Two ways to launch: `wrap` vs `run`

**`wrap` + invoke the binary directly** — leanest, for long-lived servers. Point the
launcher straight at `<venv>/bin/python-tcc-<project>`; zero per-launch overhead,
offline-safe. (Re-run `wrap` after `uv sync`.)

**`tcc-venv run` — wrap-on-demand, then run anything under the identity.** It builds and
signs the launcher if missing, then execs your command as the TCC-responsible parent.
The command can be `uv run …` (the launcher disclaims first, so identity still attaches
to the stable binary), so you get uv's auto-sync without losing the grant:

```bash
tcc-venv run --cd-to-project uv run --frozen my_app.py
# or a module:  tcc-venv run -m my_app
# in a shebang (note env -S for the multi-token line):
#!/usr/bin/env -S uvx tcc-venv run --cd-to-project uv run --frozen
```

`run` discovers the nearest `.venv` from the cwd; pass `--venv DIR` for split layouts or
an unpredictable cwd. `--cd-to-project` runs from the venv's parent. The convenience
costs `uvx`/`uv` startup per launch — use the direct-binary path when latency matters.

## Commands

| Command | What it does |
| --- | --- |
| `tcc-venv wrap [venv ...]` | Install/refresh the launcher (idempotent). Defaults to `./.venv`. Accepts multiple venvs. |
| `tcc-venv wrap --rebuild`  | Force a fresh build + sign (new cdhash — you'll need to re-grant FDA). |
| `tcc-venv run [--cd-to-project] [--venv DIR] CMD…` | Wrap-on-demand, then run `CMD` under the stable identity (e.g. `uv run …`). |
| `tcc-venv status [venv]`   | Show the installed shim, target, cdhash, and expected identifier. |

Both commands accept `--identifier-prefix PREFIX` (default `local.tcc-venv`).

The identifier is `<prefix>.<project>.<hash8>`, where `<prefix>` defaults to
`local.tcc-venv` (override with `--identifier-prefix` or `$TCC_VENV_IDENTIFIER_PREFIX`)
and `<hash8>` is derived from the venv's real path so two projects with the same name
never share a grant.

## How it works (short version)

1. Compile `trampoline.c` once per architecture (cached, unsigned).
2. Copy it to `<venv>/bin/python-tcc-<project>` and ad-hoc codesign it with a stable
   `--identifier`.
3. Cache the **signed bytes** by `identifier` + source tag; on re-wrap, copy them back
   so the cdhash is byte-identical regardless of codesign version drift. (Upgrading
   `tcc-venv` to a changed trampoline changes the tag → new cdhash → re-grant once.)
4. Symlink `python-tcc -> python-tcc-<project>`.

At runtime the trampoline resolves its own path → venv, refuses to run if a stray
`$VIRTUAL_ENV` disagrees, re-spawns a self-responsible copy of itself (disclaim
bootstrap) so its identity owns the grant under any launcher, optionally `cd`s per
`$TCC_VENV_CHDIR`, `posix_spawn`s the venv's `python`, forwards signals, and returns
the child's exit status (`128 + signo` on signal death). On non-macOS it's a plain
`exec` of the venv python with no signing.

See [`AGENTS.md`](AGENTS.md) for the full design and invariants.

## License

MIT — see [LICENSE](LICENSE).
