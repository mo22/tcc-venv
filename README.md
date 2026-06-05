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
attributes everything to *it*. Re-running `wrap` after `uv sync` restores the
**identical** signed bytes from a local cache, so the cdhash — and the grant — is
unchanged.

### What it is *not*

It does **not** bypass or weaken TCC. You still grant access explicitly in System
Settings; this only stops the identity from moving out from under that grant. It
relies on *undocumented* TCC responsible-process inheritance and ad-hoc cdhash
determinism, both of which can change across macOS releases — treat it as unofficial
and verify on the macOS versions you ship to.

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

## Commands

| Command | What it does |
| --- | --- |
| `tcc-venv wrap [venv ...]` | Install/refresh the launcher (idempotent). Defaults to `./.venv`. Accepts multiple venvs. |
| `tcc-venv wrap --rebuild`  | Force a fresh build + sign (new cdhash — you'll need to re-grant FDA). |
| `tcc-venv status [venv]`   | Show the installed shim, target, cdhash, and expected identifier. |

The identifier is `ai.mxs.tcc.<project>.<hash8>`, where `<hash8>` is derived from the
venv's real path so two projects with the same name never share a grant.

## How it works (short version)

1. Compile `trampoline.c` once per architecture (cached, unsigned).
2. Copy it to `<venv>/bin/python-tcc-<project>` and ad-hoc codesign it with a stable
   `--identifier`.
3. Cache the **signed bytes** by identifier; on re-wrap, copy them back so the cdhash
   is byte-identical regardless of codesign version drift.
4. Symlink `python-tcc -> python-tcc-<project>`.

At runtime the trampoline resolves its own path → venv, refuses to run if a stray
`$VIRTUAL_ENV` disagrees, `posix_spawn`s the venv's `python`, forwards signals, and
returns the child's exit status (`128 + signo` on signal death). On non-macOS it's a
plain `exec` of the venv python with no signing.

See [`AGENTS.md`](AGENTS.md) for the full design and invariants.

## License

MIT — see [LICENSE](LICENSE).
