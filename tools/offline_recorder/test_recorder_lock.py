#!/usr/bin/env python3
"""Integration tests for the offline recorder's process lock."""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent


class RecorderLockTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.app = self.root / "app"
        self.data = self.root / "data"
        self.volatile = self.root / "tmp"
        self.bin = self.root / "bin"
        for directory in (self.app, self.data, self.volatile, self.bin):
            directory.mkdir()

        shutil.copy2(HERE / "main.sh", self.app / "main.sh")
        (self.app / "recorder.conf").write_text(
            "seconds=1\n"
            "mode=full180\n"
            "exposure_us=500\n"
            "gain=1024\n"
            "wb_gains=0.0682,0,0,0.04897\n"
            "save_sample_frame=false\n"
            f"data_root={self.data}\n"
            "min_free_mb=1\n"
            "session_label=test\n"
        )
        (self.app / "confirm_start.py").write_text(
            "import os, sys, time\n"
            "from pathlib import Path\n"
            "time.sleep(float(os.environ.get('CONFIRM_DELAY_SECONDS', '0')))\n"
            "output = Path(sys.argv[sys.argv.index('--output') + 1])\n"
            "output.write_text('seconds=1\\nexposure_us=500\\ngain=1024\\n')\n"
        )
        (self.app / "show_status.py").write_text("")
        (self.app / "direct_record").write_text(
            "#!/bin/sh\n"
            "printf x > record.h264\n"
            "printf x > frames.csv\n"
            "printf '{\"exit_code\":0}' > capture.json\n"
        )
        (self.bin / "sleep").write_text("#!/bin/sh\nexit 0\n")
        for executable in (self.app / "main.sh", self.app / "direct_record",
                           self.bin / "sleep"):
            executable.chmod(0o755)

        self.environment = os.environ.copy()
        self.environment["PATH"] = f"{self.bin}:{self.environment['PATH']}"
        self.environment["TMPDIR"] = str(self.volatile)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_app(self, delay: float = 0) -> subprocess.CompletedProcess[str]:
        environment = self.environment.copy()
        environment["CONFIRM_DELAY_SECONDS"] = str(delay)
        return subprocess.run(
            [str(self.app / "main.sh")],
            text=True,
            capture_output=True,
            env=environment,
            timeout=10,
            check=False,
        )

    def lock_is_available(self) -> bool:
        result = subprocess.run(
            ["flock", "-n", str(self.data / ".recording.lock"), "true"],
            capture_output=True,
            check=False,
        )
        return result.returncode == 0

    def wait_for_lock(self, process: subprocess.Popen[str]) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail(f"recorder exited before holding lock: {process.returncode}")
            if (self.data / ".recording.lock").is_file() and not self.lock_is_available():
                return
            time.sleep(0.02)
        self.fail("recorder did not acquire lock")

    def wait_for_unlock(self) -> None:
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.lock_is_available():
                return
            time.sleep(0.02)
        self.fail("recorder lock remained held after the process group exited")

    def test_headless_bypasses_screen_and_honors_explicit_parameters(self) -> None:
        (self.app / "confirm_start.py").write_text("raise RuntimeError('screen must not open')\n")
        (self.app / "show_status.py").write_text("raise RuntimeError('display must not open')\n")
        result = subprocess.run(
            [str(self.app / "main.sh"), '--headless', '--seconds', '2',
             '--exposure-us', '800', '--gain', '2048', '--label', 'ssh_test'],
            env=self.environment, text=True, capture_output=True, timeout=10,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn('screen must not open', result.stderr)
        self.assertNotIn('display must not open', result.stderr)
        import json
        folder = self.data / 'clip_000001_ssh_test'
        context = json.loads((folder / 'record_context.json').read_text())
        self.assertEqual((2, 800, 2048), (context['seconds'], context['requested_exposure_us'], context['requested_gain']))
        self.assertTrue((folder / '.complete').exists())

    def test_empty_legacy_directory_is_recovered(self) -> None:
        (self.data / ".recording.lock").mkdir()

        result = self.run_app()

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Recovered stale legacy recorder lock", result.stdout)
        self.assertTrue((self.data / ".recording.lock").is_file())
        self.assertTrue((self.data / "clip_000001_test" / ".complete").is_file())
        self.assertTrue(self.lock_is_available())

    def test_lock_releases_after_forced_process_group_exit(self) -> None:
        environment = self.environment.copy()
        environment["CONFIRM_DELAY_SECONDS"] = "30"
        first = subprocess.Popen(
            [str(self.app / "main.sh")],
            text=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=environment,
            start_new_session=True,
        )
        try:
            self.wait_for_lock(first)
            second = self.run_app()
            self.assertNotEqual(0, second.returncode)
            self.assertIn("Recorder is already active", second.stderr)

            os.killpg(first.pid, signal.SIGKILL)
            first.wait(timeout=5)
            self.wait_for_unlock()

            third = self.run_app()
            self.assertEqual(0, third.returncode, third.stderr)
            self.assertTrue((self.data / "clip_000001_test" / ".complete").is_file())
        finally:
            if first.poll() is None:
                os.killpg(first.pid, signal.SIGKILL)
                first.wait(timeout=5)

    def test_long_recording_space_check_precedes_clip_reservation(self) -> None:
        (self.app / "confirm_start.py").write_text(
            "import sys\nfrom pathlib import Path\n"
            "Path(sys.argv[sys.argv.index('--output') + 1]).write_text('seconds=600\\nexposure_us=500\\ngain=1024\\n')\n"
        )
        (self.bin / "df").write_text(
            "#!/bin/sh\nprintf 'Filesystem 1024-blocks Used Available Capacity Mounted\\nmock 9999999 0 600000 1%% /\\n'\n"
        )
        (self.bin / "df").chmod(0o755)
        result = self.run_app()
        self.assertNotEqual(0, result.returncode)
        self.assertIn("Not enough space for selected duration", result.stderr)
        self.assertFalse((self.data / "next_clip_id").exists())
        self.assertFalse(list(self.data.glob("clip_*")))

    def test_forward_sequence_gap_is_saved_with_explicit_warning(self) -> None:
        status_log = self.root / "status.log"
        self.environment["STATUS_LOG"] = str(status_log)
        (self.app / "show_status.py").write_text(
            "import os, sys\n"
            "from pathlib import Path\n"
            "with Path(os.environ['STATUS_LOG']).open('a') as stream:\n"
            "    stream.write('|'.join(sys.argv[1:]) + '\\n')\n"
        )
        (self.app / "direct_record").write_text(
            "#!/bin/sh\n"
            "printf x > record.h264\n"
            "printf x > frames.csv\n"
            "printf '{\"exit_code\":0,\"sequence_gap_events\":1,"
            "\"sequence_missing\":2,\"mipi_errors_max\":2,"
            "\"recovered_mipi_error\":true}' > capture.json\n"
        )
        (self.app / "direct_record").chmod(0o755)

        result = self.run_app()

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertTrue((self.data / "clip_000001_test" / ".complete").is_file())
        self.assertFalse((self.data / "clip_000001_test.failed").exists())
        self.assertIn(
            "SAVED WITH GAPS 000001|missing: 2 | MIPI: 2|warning|5",
            status_log.read_text(),
        )


if __name__ == "__main__":
    unittest.main()
