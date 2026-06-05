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
CACHE_DIR = Path(os.environ.get("TCC_VENV_CACHE", Path.home() / ".cache" / "tcc-venv"))
SOURCE = Path(__file__).with_name("trampoline.c")

# Generic, non-personal default. The reverse-DNS-ish form is only a codesign
# identifier string; override per-org with --identifier-prefix / the env var.
DEFAULT_IDENTIFIER_PREFIX = "local.tcc-venv"
CFLAGS = ["-Wall", "-Wextra", "-O2"]


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #


def _die(msg: str, code: int = 1) -> NoReturn:
    print(f"tcc-venv: {msg}", file=sys.stderr)
    raise SystemExit(code)


def _tool(name: str) -> str:
    """Resolve a build tool, preferring the system copy in /usr/bin over $PATH
    so a shadowed `codesign`/`cc` can't slip into a security-sensitive launcher."""
    system = Path("/usr/bin") / name
    if system.exists():
        return str(system)
    found = shutil.which(name)
    if found:
        return found
    _die(f"required tool not found: {name}")


def _cc() -> str:
    """C compiler. Prefer $CC, then the absolute /usr/bin/cc — the system driver
    shim that auto-injects the active SDK sysroot (the bare `xcrun --find cc`
    toolchain clang does not, so it can't find <errno.h>). Fall back to $PATH."""
    env_cc = os.environ.get("CC")
    if env_cc:
        return env_cc
    if Path("/usr/bin/cc").exists():
        return "/usr/bin/cc"
    found = shutil.which("cc")
    if not found:
        _die("no C compiler (cc) found — install Xcode command line tools.")
    return found


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


def _identifier_prefix(args: argparse.Namespace) -> str:
    return (
        getattr(args, "identifier_prefix", None)
        or os.environ.get("TCC_VENV_IDENTIFIER_PREFIX")
        or DEFAULT_IDENTIFIER_PREFIX
    )


def _identity(venv: Path, prefix: str) -> tuple[str, str]:
    """(filename, identifier). Identifier includes a hash of the venv realpath so
    two projects with the same friendly name never share a TCC grant (Codex #6)."""
    name = _project_name(venv)
    digest = hashlib.sha256(str(venv).encode()).hexdigest()[:8]
    return f"python-tcc-{name}", f"{prefix}.{name}.{digest}"


def _arch() -> str:
    return platform.machine() or "unknown"


def _source_tag() -> str:
    """A short hash of the trampoline source + compiler flags. Two tcc-venv versions
    with different trampoline bytes get different tags, so the per-arch unsigned and
    per-identity signed caches both bust on a real source change (Codex #3: mtime is
    unreliable across reinstalls)."""
    return hashlib.sha256(
        SOURCE.read_bytes() + b"\0" + " ".join(CFLAGS).encode()
    ).hexdigest()[:16]


def _build_unsigned() -> Path:
    """Compile the trampoline; cache the unsigned binary keyed by the source tag."""
    key = _source_tag()
    out = CACHE_DIR / "unsigned" / _arch() / f"{key}.bin"
    if out.exists():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    subprocess.run([_cc(), *CFLAGS, str(SOURCE), "-o", str(tmp)], check=True)
    os.replace(tmp, out)
    return out


def _codesign_show(path: Path) -> dict[str, str]:
    res = subprocess.run(
        [_tool("codesign"), "-dvvv", str(path)], capture_output=True, text=True
    )
    info: dict[str, str] = {}
    for line in (res.stderr + res.stdout).splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            info.setdefault(key.strip(), value.strip())
    return info


def _cdhash(path: Path) -> str:
    return _codesign_show(path).get("CDHash", "?")


def _verify(path: Path, identifier: str) -> bool:
    """A signed binary is good only if it passes codesign --verify, carries the
    expected identifier, and has a real cdhash (Codex #5)."""
    res = subprocess.run(
        [_tool("codesign"), "--verify", "--strict", str(path)],
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        return False
    info = _codesign_show(path)
    return info.get("Identifier") == identifier and info.get("CDHash", "?") not in (
        "",
        "?",
    )


def _atomic_install(
    installed: Path, source: Path, identifier: str, *, sign: bool
) -> bool:
    """Stage `source` into a temp file beside `installed`, optionally codesign it,
    verify it, then atomically swap it in (Codex #4 — never expose an unsigned or
    half-written binary). Returns False if the result fails verification."""
    tmp = installed.with_name(f".{installed.name}.tmp")
    try:
        shutil.copyfile(source, tmp)
        tmp.chmod(0o755)
        if sign:
            # Capture codesign's chatter (e.g. "replacing existing signature",
            # which arm64 always emits — the linker ad-hoc signs at link time);
            # surface it only if signing actually fails.
            res = subprocess.run(
                [
                    _tool("codesign"),
                    "--force",
                    "--sign",
                    "-",
                    "--identifier",
                    identifier,
                    str(tmp),
                ],
                capture_output=True,
                text=True,
            )
            if res.returncode != 0:
                _die(f"codesign failed for {installed}:\n{res.stderr.strip()}")
        if not _verify(tmp, identifier):
            return False
        os.replace(tmp, installed)
        return True
    finally:
        tmp.unlink(missing_ok=True)


def _symlink(link: Path, target: str) -> None:
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(target)


# --------------------------------------------------------------------------- #
# commands
# --------------------------------------------------------------------------- #


def cmd_wrap(args: argparse.Namespace) -> None:
    prefix = _identifier_prefix(args)
    # Compute once so every venv in a single invocation is keyed consistently, even
    # if the source were edited mid-run.
    source_tag = _source_tag()
    arch = _arch()
    for raw in args.venv or [None]:
        venv = _venv_dir(raw)
        bindir = venv / "bin"
        shim = bindir / "python-tcc"

        if not IS_MACOS:
            _symlink(shim, "python")
            print(f"{shim} -> python   (non-macOS passthrough)")
            continue

        filename, identifier = _identity(venv, prefix)
        installed = bindir / filename
        # Key the signed cache by identifier + arch + source tag: re-wrap after
        # `uv sync` restores byte-identical bytes (grant persists), but upgrading
        # tcc-venv to a changed trampoline busts the cache → fresh build+sign → new
        # cdhash (a one-time FDA re-grant, which a genuinely different binary requires
        # anyway). Arch is in the key because signed bytes are arch-specific.
        signed_cache = CACHE_DIR / "signed" / f"{identifier}.{arch}.{source_tag}"

        # Reuse the exact signed bytes if we have them (Codex #7: copy-back beats
        # re-signing, so the cdhash — and thus the TCC grant — is identical by
        # construction across uv sync / codesign version drift). The restore is
        # verified, not trusted, so a corrupt/mismatched cache falls through to a
        # fresh build (Codex #5).
        restored = False
        if signed_cache.exists() and not args.rebuild:
            restored = _atomic_install(installed, signed_cache, identifier, sign=False)

        if not restored:
            unsigned = _build_unsigned()
            if not _atomic_install(installed, unsigned, identifier, sign=True):
                _die(f"codesign verification failed for {installed}")
            signed_cache.parent.mkdir(parents=True, exist_ok=True)
            cache_tmp = signed_cache.with_suffix(".tmp")
            shutil.copyfile(installed, cache_tmp)
            os.replace(cache_tmp, signed_cache)

        _symlink(shim, filename)

        print(f"wrapped {venv}")
        print(f"  binary:     {installed}")
        print(f"  shim:       {shim} -> {filename}")
        print(f"  identifier: {identifier}")
        print(f"  cdhash:     {_cdhash(installed)}")
        print("  run your app through this stable binary (not uv/uvx), e.g.:")
        print(f"    {installed} your_app.py")
        print(f"    {installed} -m your_module")
        print(
            "  set TCC_VENV_CHDIR=1 to run from the project root (or a path to cd there)"
        )
    if IS_MACOS:
        print(
            "\nGrant the binary Full Disk Access ONCE:\n"
            "  System Settings -> Privacy & Security -> Full Disk Access -> add the\n"
            "  python-tcc-<project> binary above. Automation/EventKit prompts appear\n"
            "  on first use. Re-running `tcc-venv wrap` after `uv sync` restores the\n"
            "  identical identity, so the grant persists. Upgrading tcc-venv to a new\n"
            "  trampoline changes the cdhash — you'll re-grant once when that happens."
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
        _, identifier = _identity(venv, _identifier_prefix(args))
        print(f"  binary: {real}")
        print(f"  cdhash: {_cdhash(real)}")
        print(f"  expect: {identifier}")


def _add_prefix_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--identifier-prefix",
        default=None,
        help="codesign identifier prefix (default: $TCC_VENV_IDENTIFIER_PREFIX or "
        f"{DEFAULT_IDENTIFIER_PREFIX!r}); identifier is <prefix>.<project>.<hash8>",
    )


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
    _add_prefix_arg(p_wrap)
    p_wrap.set_defaults(func=cmd_wrap)

    p_status = sub.add_parser("status", help="show the wrapper installed in a venv")
    p_status.add_argument("venv", nargs="?", help="venv dir; default ./.venv")
    _add_prefix_arg(p_status)
    p_status.set_defaults(func=cmd_status)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
