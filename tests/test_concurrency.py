"""Concurrency regression tests for `wrap`.

Overlapping invocations are normal, not exotic: several launchd daemons share a
venv and all start in the same second at boot. So every staging path must be
private to the writing process, and `<venv>/bin/python-tcc` must be continuously
resolvable. Both properties regressed once — a fixed `.tmp` name plus
`finally: tmp.unlink()`, and an unlink-then-symlink swap — and cost a boot
(2026-07-31), hence these tests.

Note the third case, `two_venvs`: `_build_unsigned` stages into the *machine-wide*
cache, so unrelated projects race there too. A same-venv test cannot see it.

macOS only — off Darwin `wrap` is a plain symlink with none of this machinery.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"

# Each round is an independent coin flip, so a few rounds turn a flaky race into a
# reliable signal without making the suite slow.
ROUNDS = 3


def _fake_venv(root: Path, prompt: str) -> Path:
    """A venv skeleton. `wrap` needs only pyvenv.cfg and bin/, never an interpreter,
    so we skip the cost of building a real one."""
    venv = root / ".venv"
    (venv / "bin").mkdir(parents=True)
    (venv / "pyvenv.cfg").write_text(f"home = /usr/bin\nprompt = '{prompt}'\n")
    return venv


def _cdhash(binary: Path) -> str:
    res = subprocess.run(
        ["/usr/bin/codesign", "-dvvv", str(binary)],
        capture_output=True,
        text=True,
        check=False,
    )
    m = re.search(r"^CDHash=(\S+)", res.stderr + res.stdout, re.MULTILINE)
    return m.group(1) if m else ""


@unittest.skipUnless(sys.platform == "darwin", "wrap only signs on macOS")
class ConcurrentWrapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="tcc-venv-test."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.cache = self.tmp / "cache"
        self.env = {
            **os.environ,
            "PYTHONPATH": str(SRC),
            "TCC_VENV_CACHE": str(self.cache),
        }

    def _wrap_many(self, venvs: list[Path], n: int) -> list[tuple[int, str]]:
        """Launch n wraps at once (cycling over venvs); return the failures."""
        procs = [
            subprocess.Popen(
                [
                    sys.executable,
                    "-m",
                    "tcc_venv.cli",
                    "wrap",
                    str(venvs[i % len(venvs)]),
                ],
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            for i in range(n)
        ]
        results = [(p.wait(), p.communicate()[0] or "") for p in procs]
        return [(rc, out) for rc, out in results if rc != 0]

    def _assert_all_ok(self, failures: list[tuple[int, str]], label: str) -> None:
        if failures:
            rc, out = failures[0]
            self.fail(
                f"{label}: {len(failures)} concurrent wrap(s) failed "
                f"(first rc={rc}):\n{out.strip()}"
            )

    def test_already_wrapped(self) -> None:
        """Re-running against a correctly wrapped venv: nothing to do, and the fast
        path must make that literally true — no writes means nothing to race."""
        venv = _fake_venv(self.tmp / "warm", "warmproj")
        self._assert_all_ok(self._wrap_many([venv], 1), "seed")
        binary = venv / "bin" / "python-tcc-warmproj"
        before = binary.stat().st_mtime_ns
        for r in range(ROUNDS):
            self._assert_all_ok(self._wrap_many([venv], 6), f"warm round {r}")
        self.assertEqual(
            before, binary.stat().st_mtime_ns, "fast path rewrote the binary"
        )

    def test_restore_after_uv_sync(self) -> None:
        """`uv sync` deletes the binary but leaves the signed cache, so concurrent
        launches all race to restore it. This is the exact shape of the boot that
        broke on 2026-07-31, and the only test that covers the restore path — the
        fast path deliberately skips it when the binary is already in place."""
        venv = _fake_venv(self.tmp / "sync", "syncproj")
        self._assert_all_ok(self._wrap_many([venv], 1), "seed")
        for r in range(ROUNDS):
            for stale in (venv / "bin").glob("python-tcc*"):
                stale.unlink()
            self._assert_all_ok(self._wrap_many([venv], 6), f"restore round {r}")

    def test_cold_cache(self) -> None:
        """No cache at all: every run compiles, signs and populates it."""
        venv = _fake_venv(self.tmp / "cold", "coldproj")
        for r in range(ROUNDS):
            shutil.rmtree(self.cache, ignore_errors=True)
            self._assert_all_ok(self._wrap_many([venv], 4), f"cold round {r}")

    def test_two_venvs_share_the_build_cache(self) -> None:
        """Unrelated projects still collide in the machine-wide unsigned cache."""
        a = _fake_venv(self.tmp / "alpha", "alphaproj")
        b = _fake_venv(self.tmp / "beta", "betaproj")
        for r in range(ROUNDS):
            shutil.rmtree(self.cache, ignore_errors=True)
            self._assert_all_ok(self._wrap_many([a, b], 4), f"two-venv round {r}")

    def test_cdhash_is_stable_across_concurrent_wraps(self) -> None:
        """Racing runs must not leave a different identity behind than a lone run."""
        venv = _fake_venv(self.tmp / "hash", "hashproj")
        self._assert_all_ok(self._wrap_many([venv], 1), "seed")
        binary = venv / "bin" / "python-tcc-hashproj"
        serial = _cdhash(binary)
        self.assertTrue(serial, "no cdhash after a plain wrap")
        self._assert_all_ok(self._wrap_many([venv], 6), "concurrent")
        self.assertEqual(serial, _cdhash(binary), "cdhash changed — TCC grant lost")

    def test_shim_never_disappears(self) -> None:
        """A daemon exec'ing python-tcc mid-wrap must never see ENOENT."""
        venv = _fake_venv(self.tmp / "shim", "shimproj")
        self._assert_all_ok(self._wrap_many([venv], 1), "seed")
        shim = venv / "bin" / "python-tcc"
        gaps: list[int | None] = []
        stop = threading.Event()

        def poll() -> None:
            while not stop.is_set():
                try:
                    os.readlink(shim)
                except OSError as e:
                    gaps.append(e.errno)

        watcher = threading.Thread(target=poll, daemon=True)
        watcher.start()
        try:
            self._assert_all_ok(self._wrap_many([venv], 6), "concurrent")
        finally:
            stop.set()
            watcher.join(timeout=5)
        self.assertEqual(gaps, [], f"shim was unresolvable {len(gaps)}x mid-wrap")


if __name__ == "__main__":
    unittest.main()
