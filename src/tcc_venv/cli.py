"""tcc-venv — install a stable, codesigned TCC launcher into a uv/venv.

See AGENTS.md for the design. In short: on macOS each wrapped venv gets

    <venv>/bin/python-tcc-<project>   # signed trampoline (stable per-project identity)
    <venv>/bin/python-tcc -> python-tcc-<project>   # uniform shim used in shebangs/control

so TCC attributes Full Disk Access / Automation to `python-tcc-<project>` instead of
the moving Homebrew-uv / version-pinned-python paths. On non-macOS, `python-tcc` is a
plain symlink to `python` (no TCC, pure passthrough).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

IS_MACOS = sys.platform == "darwin"
CACHE_DIR = Path(
    os.environ.get("TCC_VENV_CACHE", Path.home() / ".cache" / "tcc-venv")
)
SOURCE = Path(__file__).with_name("trampoline.c")


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #


def _die(msg: str, code: int = 1) -> NoReturn:
    print(f"tcc-venv: {msg}", file=sys.stderr)
    raise SystemExit(code)


def _venv_dir(arg: str | None) -> Path:
    """Resolve the target venv. Defaults to ./.venv, accepts a venv or its bin/."""
    candidate = Path(arg).expanduser() if arg else Path.cwd() / ".venv"
    candidate = candidate.resolve()
    if candidate.name == "bin":
        candidate = candidate.parent
    if not (candidate / "pyvenv.cfg").exists():
        _die(f"{candidate} is not a venv (no pyvenv.cfg). Run `uv sync` first.")
    return candidate


def _project_name(venv: Path) -> str:
    """Friendly project name: pyvenv.cfg `prompt`, else the venv's parent dir name."""
    name = ""
    cfg = venv / "pyvenv.cfg"
    for line in cfg.read_text().splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            if key.strip() == "prompt":
                name = value.strip().strip("'\"")
                break
    if not name:
        name = venv.parent.name
    slug = re.sub(r"[^A-Za-z0-9._-]+", "-", name).strip("-._") or "venv"
    return slug


def _identity(venv: Path) -> tuple[str, str]:
    """(filename, identifier). Identifier includes a hash of the venv realpath so
    two projects with the same friendly name never share a TCC grant (Codex #6)."""
    name = _project_name(venv)
    digest = hashlib.sha256(str(venv).encode()).hexdigest()[:8]
    return f"python-tcc-{name}", f"ai.mxs.tcc.{name}.{digest}"


def _arch() -> str:
    return platform.machine() or "unknown"


def _build_unsigned() -> Path:
    """Compile the trampoline once per arch; cache the unsigned binary."""
    out = CACHE_DIR / "unsigned" / _arch() / "python-tcc"
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists() and out.stat().st_mtime >= SOURCE.stat().st_mtime:
        return out
    cc = shutil.which("cc")
    if not cc:
        _die("no C compiler (cc) found — install Xcode command line tools.")
    tmp = out.with_suffix(".tmp")
    subprocess.run(
        [cc, "-Wall", "-Wextra", "-O2", str(SOURCE), "-o", str(tmp)],
        check=True,
    )
    tmp.replace(out)
    return out


def _cdhash(path: Path) -> str:
    res = subprocess.run(
        ["codesign", "-dvvv", str(path)], capture_output=True, text=True
    )
    for line in (res.stderr + res.stdout).splitlines():
        if line.startswith("CDHash="):
            return line.split("=", 1)[1].strip()
    return "?"


def _symlink(link: Path, target: str) -> None:
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(target)


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #


def cmd_wrap(args: argparse.Namespace) -> None:
    for raw in args.venv or [None]:
        venv = _venv_dir(raw)
        bindir = venv / "bin"
        shim = bindir / "python-tcc"

        if not IS_MACOS:
            _symlink(shim, "python")
            print(f"{shim} -> python   (non-macOS passthrough)")
            continue

        filename, identifier = _identity(venv)
        installed = bindir / filename
        signed_cache = CACHE_DIR / "signed" / identifier

        # Reuse the exact signed bytes if we have them (Codex #7: copy-back beats
        # re-signing, so the cdhash — and thus the TCC grant — is identical by
        # construction across uv sync / codesign version drift).
        if signed_cache.exists() and not args.rebuild:
            shutil.copyfile(signed_cache, installed)
        else:
            shutil.copyfile(_build_unsigned(), installed)
            installed.chmod(0o755)
            subprocess.run(
                ["codesign", "--force", "--sign", "-", "--identifier", identifier,
                 str(installed)],
                check=True,
            )
            signed_cache.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(installed, signed_cache)
        installed.chmod(0o755)
        _symlink(shim, filename)

        print(f"wrapped {venv}")
        print(f"  binary:     {installed}")
        print(f"  shim:       {shim} -> {filename}")
        print(f"  identifier: {identifier}")
        print(f"  cdhash:     {_cdhash(installed)}")
    if IS_MACOS:
        print(
            "\nGrant the binary Full Disk Access ONCE:\n"
            "  System Settings -> Privacy & Security -> Full Disk Access -> add the\n"
            "  python-tcc-<project> binary above. Automation/EventKit prompts appear\n"
            "  on first use. Re-running `tcc-venv wrap` after `uv sync` restores the\n"
            "  identical identity, so the grant persists."
        )


def cmd_status(args: argparse.Namespace) -> None:
    venv = _venv_dir(args.venv)
    shim = venv / "bin" / "python-tcc"
    if not shim.is_symlink():
        print(f"{venv}: not wrapped")
        return
    target = os.readlink(shim)
    print(f"{venv}")
    print(f"  shim:   {shim} -> {target}")
    real = venv / "bin" / target
    if IS_MACOS and real.exists():
        _, identifier = _identity(venv)
        print(f"  binary: {real}")
        print(f"  cdhash: {_cdhash(real)}")
        print(f"  expect: {identifier}")


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="tcc-venv",
        description="Install a stable codesigned TCC launcher into a uv/venv.",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_wrap = sub.add_parser("wrap", help="install python-tcc into a venv (idempotent)")
    p_wrap.add_argument("venv", nargs="*", help="venv dir(s); default ./.venv")
    p_wrap.add_argument(
        "--rebuild",
        action="store_true",
        help="force a fresh build+sign (new cdhash — needs re-granting FDA)",
    )
    p_wrap.set_defaults(func=cmd_wrap)

    p_status = sub.add_parser("status", help="show the wrapper installed in a venv")
    p_status.add_argument("venv", nargs="?", help="venv dir; default ./.venv")
    p_status.set_defaults(func=cmd_status)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
