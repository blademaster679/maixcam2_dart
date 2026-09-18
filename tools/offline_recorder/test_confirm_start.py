#!/usr/bin/env python3
"""Host-side regression tests for the recorder confirmation gate."""
from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("confirm_start", HERE / "confirm_start.py")
confirm = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(confirm)


class PressReleaseGateTests(unittest.TestCase):
    START_RECT = (10, 10, 100, 60)
    CANCEL_RECT = (10, 90, 100, 60)

    def gate(self):
        return confirm.PressReleaseGate({
            confirm.START: self.START_RECT,
            confirm.CANCEL: self.CANCEL_RECT,
        })

    def test_deliberate_start_click(self):
        gate = self.gate()
        self.assertIsNone(gate.feed(20, 20, True, 1.0))
        self.assertEqual(confirm.START, gate.highlighted)
        self.assertEqual(confirm.START, gate.feed(20, 20, False, 1.1))

    def test_deliberate_cancel_click(self):
        gate = self.gate()
        self.assertIsNone(gate.feed(20, 100, True, 1.0))
        self.assertEqual(confirm.CANCEL, gate.feed(20, 100, False, 1.1))

    def test_accidental_and_incomplete_gestures_are_ignored(self):
        cases = [
            [(0, 0, True, 1.0), (0, 0, False, 1.1)],
            [(20, 20, True, 2.0), (20, 20, False, 2.01)],
            [(20, 20, True, 3.0), (200, 200, False, 3.1)],
        ]
        for events in cases:
            gate = self.gate()
            self.assertTrue(all(gate.feed(*event) is None for event in events))

    def test_confirmation_precedes_clip_reservation(self):
        script = (HERE / "main.sh").read_text()
        self.assertIn("'') shift ;; # The MaixAPP launcher supplies one empty argument.",
                      script)
        self.assertLess(script.index('"$APP_DIR/confirm_start.py"'),
                        script.index("next_id=$((clip_id + 1))"))
        self.assertLess(script.index("10)"),
                        script.index("next_id=$((clip_id + 1))"))

    def test_settings_adjust_reset_and_persist(self):
        defaults = {"seconds": 10, "exposure_us": 500, "gain": 0}
        settings = confirm.RecorderSettings(defaults)
        self.assertEqual(1024, settings.values["gain"])
        self.assertTrue(settings.adjust("exposure_up"))
        self.assertEqual(800, settings.values["exposure_us"])
        self.assertTrue(settings.adjust("gain_up"))
        self.assertEqual("2.0 x", settings.display_value("gain"))
        settings.reset()
        self.assertEqual({"seconds": 10, "exposure_us": 500, "gain": 1024},
                         settings.values)

        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state.conf"
            confirm.write_settings(state, settings.values, include_schema=True)
            self.assertEqual(settings.values, confirm.read_saved_settings(state))

    def test_ten_minute_selection_and_persistence(self):
        settings = confirm.RecorderSettings({"seconds": 10, "exposure_us": 500, "gain": 1024})
        while settings.adjust("seconds_up"):
            pass
        self.assertEqual(600, settings.values["seconds"])
        self.assertEqual("10 min", settings.display_value("seconds"))
        self.assertFalse(confirm.valid_values({**settings.values, "seconds": 601}))
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "state.conf"
            confirm.write_settings(state, settings.values, include_schema=True)
            self.assertEqual(settings.values, confirm.read_saved_settings(state))

    def test_landscape_layout_has_separate_controls(self):
        rects = confirm.build_layout(640, 480)
        self.assertGreaterEqual(rects["seconds_down"][1], 60)
        self.assertLess(rects["gain_up"][1] + rects["gain_up"][3],
                        rects[confirm.START][1])
        self.assertGreater(rects[confirm.START][2], 200)

    def test_recording_profile_uses_deep_venc_fifo_without_rt_starvation(self):
        script = (HERE / "main.sh").read_text()
        self.assertIn("--venc-depth 8", script)
        self.assertIn('--max-mipi-errors "$MAX_MIPI_ERRORS"', script)
        self.assertNotIn("--realtime", script)


if __name__ == "__main__":
    unittest.main()
