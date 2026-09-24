"""Parent-death teardown regression test.

When the trampoline's parent dies — including a SIGKILL, where the forwarded-signal
path can never run — the trampoline must tear down its whole child tree and exit,
instead of lingering reparented to launchd with the payload still alive. That leak
held an instance lock for days and grew to a 103 GB footprint (incident 2026-09-24),
so this guards the fix in trampoline.c (kqueue NOTE_EXIT on the parent pid + a
layer-aware group teardown).

The shape: a throwaway parent starts the tcc-venv chain, we SIGKILL the parent, then
assert nothing tagged survives. Two chains are covered — the direct two-layer
disclaim path, and `tcc-venv run` with a payload that forks a same-group grandchild
(mirrors the incident's `uv run python`). The payload IGNORES SIGTERM, so only a
working teardown (escalating to SIGKILL on the right group) can stop it.

macOS only — off Darwin the trampoline is a plain exec with no supervisor. Stdlib
unittest, no test dependency, same as test_concurrency.py.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"

# A throwaway parent: start the chain, record its pid, then just sit. SIGKILLing
# THIS process is the "parent died" event under test.
PARENT_SRC = r"""
import os, subprocess, sys, time
markdir = os.environ["MARKDIR"]
p = subprocess.Popen(sys.argv[1:])
with open(os.path.join(markdir, "chainroot.pid"), "w") as fh:
    fh.write(str(p.pid))
while True:
    time.sleep(3600)
"""

# The payload ignores SIGTERM (the incident shape); in "fork" mode it also forks a
# child in the SAME process group, mirroring `uv run python`.
PAYLOAD_SRC = r"""
import os, signal, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
markdir = os.environ["MARKDIR"]
mode = sys.argv[2] if len(sys.argv) > 2 else "single"
def mark(name, pid):
    with open(os.path.join(markdir, name), "w") as fh:
        fh.write(str(pid))
if mode == "fork":
    if os.fork() == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        mark("child.pid", os.getpid())
        while True:
            time.sleep(3600)
    mark("payload.pid", os.getpid())
    while True:
        time.sleep(3600)
else:
    mark("payload.pid", os.getpid())
    while True:
        time.sleep(3600)
"""


@unittest.skipUnless(sys.platform == "darwin", "trampoline supervises only on macOS")
class ParentDeathTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="tcc-venv-pd."))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.env = {
            **os.environ,
            "PYTHONPATH": str(SRC),
            "TCC_VENV_CACHE": str(self.tmp / "cache"),
        }
        # A real venv — the trampoline actually spawns <venv>/bin/python.
        self.venv = self.tmp / ".venv"
        subprocess.run(
            [sys.executable, "-m", "venv", str(self.venv)],
            check=True,
            capture_output=True,
        )
        with open(self.venv / "pyvenv.cfg", "a") as fh:
            fh.write("prompt = 'pdproj'\n")
        res = subprocess.run(
            [sys.executable, "-m", "tcc_venv.cli", "wrap", str(self.venv)],
            env=self.env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(res.returncode, 0, f"wrap failed:\n{res.stderr}")
        self.trampoline = self.venv / "bin" / "python-tcc-pdproj"
        self.assertTrue(self.trampoline.exists(), "trampoline not installed")

        self.payload = self.tmp / "payload.py"
        self.payload.write_text(PAYLOAD_SRC)
        self.parent_src = self.tmp / "parent.py"
        self.parent_src.write_text(PARENT_SRC)

    def _pgrep(self, tag: str) -> list[int]:
        res = subprocess.run(
            ["/usr/bin/pgrep", "-f", tag], capture_output=True, text=True, check=False
        )
        return [int(x) for x in res.stdout.split()]

    def _assert_torn_down(self, chain: list[str], tag: str) -> None:
        markdir = self.tmp / f"marks-{tag}"
        markdir.mkdir()
        env = {**self.env, "MARKDIR": str(markdir)}
        # Never leave a stuck payload behind, even if an assertion fails midway.
        self.addCleanup(
            subprocess.run, ["/usr/bin/pkill", "-KILL", "-f", tag], check=False
        )

        parent = subprocess.Popen(
            [sys.executable, str(self.parent_src), *chain], env=env
        )
        self.addCleanup(parent.wait)
        self.addCleanup(parent.kill)

        # Wait for the grandchild payload to come up.
        deadline = time.time() + 20
        while time.time() < deadline and not (markdir / "payload.pid").exists():
            if parent.poll() is not None:
                self.fail(f"parent exited early rc={parent.returncode}")
            time.sleep(0.1)
        self.assertTrue(
            (markdir / "payload.pid").exists(),
            f"payload never started (tree: {self._pgrep(tag)})",
        )
        self.assertTrue(self._pgrep(tag), "no tagged processes before the kill")

        # The event under test: the parent dies by SIGKILL (no chance to forward).
        parent.kill()
        parent.wait()

        # SIGTERM -> ~3 s grace -> SIGKILL, plus slack for the cascade between layers.
        deadline = time.time() + 12
        while time.time() < deadline and self._pgrep(tag):
            time.sleep(0.2)
        survivors = self._pgrep(tag)
        self.assertEqual(
            survivors,
            [],
            f"tree orphaned after parent SIGKILL: {survivors}",
        )

    def test_direct_chain_is_torn_down(self) -> None:
        """`python-tcc payload` (bootstrap + disclaimed layer, then python)."""
        tag = f"tccpd-direct-{uuid.uuid4().hex[:8]}"
        self._assert_torn_down([str(self.trampoline), str(self.payload), tag], tag)

    def test_run_with_forked_grandchild_is_torn_down(self) -> None:
        """`tcc-venv run <python> payload fork`: the extra process group below the
        disclaimed layer (like `uv run python`) must be reached by the teardown."""
        tag = f"tccpd-runfork-{uuid.uuid4().hex[:8]}"
        chain = [
            sys.executable,
            "-m",
            "tcc_venv.cli",
            "run",
            "--venv",
            str(self.venv),
            str(self.venv / "bin" / "python3"),
            str(self.payload),
            tag,
            "fork",
        ]
        self._assert_torn_down(chain, tag)


if __name__ == "__main__":
    unittest.main()
