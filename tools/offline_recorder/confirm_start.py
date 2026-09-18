#!/usr/bin/env python3
"""Touchscreen parameter panel shown before an offline recording is allocated."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import time
from typing import Dict, Optional, Tuple


START = "start"
CANCEL = "cancel"
RESET = "reset"
CANCEL_EXIT_CODE = 10
Rect = Tuple[int, int, int, int]

DURATION_PRESETS = (3, 5, 10, 15, 20, 30, 60, 120, 300, 600)
EXPOSURE_PRESETS = (100, 200, 400, 500, 800, 1200, 1600, 2400, 3200, 4800)
# AX ISP analog gain uses U22.10: 1024 is 1.0x. OS04A10 SDR is rated to 16x.
GAIN_PRESETS = (1024, 2048, 4096, 8192, 16384)

ADJUST_ACTIONS = {
    "seconds_down": ("seconds", -1),
    "seconds_up": ("seconds", 1),
    "exposure_down": ("exposure_us", -1),
    "exposure_up": ("exposure_us", 1),
    "gain_down": ("gain", -1),
    "gain_up": ("gain", 1),
}


def contains(rect: Rect, x: int, y: int) -> bool:
    left, top, width, height = rect
    return left <= x < left + width and top <= y < top + height


def valid_values(values: Dict[str, int]) -> bool:
    return (
        1 <= values.get("seconds", 0) <= 600
        and 1 <= values.get("exposure_us", 0) <= 5554
        and 1024 <= values.get("gain", 0) <= 16384
    )


def read_saved_settings(path: Path) -> Optional[Dict[str, int]]:
    try:
        parsed: Dict[str, int] = {}
        for raw_line in path.read_text().splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            key, separator, value = line.partition("=")
            if not separator or key not in {"schema_version", "seconds", "exposure_us", "gain"}:
                return None
            if key != "schema_version":
                parsed[key] = int(value)
        return parsed if set(parsed) == {"seconds", "exposure_us", "gain"} and valid_values(parsed) else None
    except (OSError, ValueError):
        return None


def write_settings(path: Path, values: Dict[str, int], include_schema: bool) -> None:
    if not valid_values(values):
        raise ValueError("invalid recorder settings")
    lines = []
    if include_schema:
        lines.append("schema_version=1")
    lines.extend([
        f"seconds={values['seconds']}",
        f"exposure_us={values['exposure_us']}",
        f"gain={values['gain']}",
    ])
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w") as output:
        output.write("\n".join(lines) + "\n")
        output.flush()
        os.fsync(output.fileno())
    os.replace(str(temporary), str(path))


class RecorderSettings:
    PRESETS = {
        "seconds": DURATION_PRESETS,
        "exposure_us": EXPOSURE_PRESETS,
        "gain": GAIN_PRESETS,
    }

    def __init__(self, defaults: Dict[str, int], saved: Optional[Dict[str, int]] = None):
        normalized = dict(defaults)
        if normalized.get("gain") == 0:
            normalized["gain"] = 1024
        if not valid_values(normalized):
            raise ValueError("invalid initial recorder settings")
        self.defaults = normalized
        self.values = dict(saved) if saved is not None and valid_values(saved) else dict(normalized)
        self.options: Dict[str, Tuple[int, ...]] = {}
        for field, presets in self.PRESETS.items():
            self.options[field] = tuple(sorted(set(presets + (self.values[field], normalized[field]))))

    def adjust(self, action: str) -> bool:
        if action not in ADJUST_ACTIONS:
            return False
        field, direction = ADJUST_ACTIONS[action]
        options = self.options[field]
        current = options.index(self.values[field])
        target = max(0, min(len(options) - 1, current + direction))
        changed = target != current
        self.values[field] = options[target]
        return changed

    def reset(self) -> None:
        self.values = dict(self.defaults)

    def display_value(self, field: str) -> str:
        value = self.values[field]
        if field == "seconds":
            return f"{value // 60} min" if value >= 60 and value % 60 == 0 else f"{value} s"
        if field == "exposure_us":
            return f"{value} us"
        if field == "gain":
            return f"{value / 1024.0:.1f} x"
        raise KeyError(field)


class PressReleaseGate:
    """Accept a deliberate click only when press and release hit one button."""

    def __init__(self, rects: Dict[str, Rect], min_hold_s: float = 0.06):
        self._rects = dict(rects)
        self._min_hold_s = min_hold_s
        self._pressed = False
        self._target: Optional[str] = None
        self._pressed_at = 0.0

    @property
    def highlighted(self) -> Optional[str]:
        return self._target if self._pressed else None

    def _hit(self, x: int, y: int) -> Optional[str]:
        for name, rect in self._rects.items():
            if contains(rect, x, y):
                return name
        return None

    def feed(self, x: int, y: int, pressed: bool, now_s: float) -> Optional[str]:
        pressed = bool(pressed)
        if pressed and not self._pressed:
            self._target = self._hit(x, y)
            self._pressed_at = now_s
        elif not pressed and self._pressed:
            target = self._target
            self._target = None
            held_long_enough = now_s - self._pressed_at >= self._min_hold_s
            if target is not None and held_long_enough and self._hit(x, y) == target:
                self._pressed = pressed
                return target
        self._pressed = pressed
        return None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--clip", required=True)
    parser.add_argument("--seconds", required=True, type=int)
    parser.add_argument("--exposure-us", required=True, type=int)
    parser.add_argument("--gain", required=True, type=int)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    return parser.parse_args()


def build_layout(width: int, height: int) -> Dict[str, Rect]:
    margin = max(16, width // 36)
    row_height = max(48, min(56, height // 8))
    row_gap = max(8, height // 60)
    first_row = max(68, height // 7)
    controls_left = max(170, int(width * 0.32))
    side_width = max(56, int(width * 0.105))
    plus_x = width - margin - side_width
    value_x = controls_left + side_width + 8
    value_width = plus_x - value_x - 8
    if value_width < 110:
        raise ValueError("display is too narrow for recorder controls")

    rects: Dict[str, Rect] = {}
    for row, field in enumerate(("seconds", "exposure", "gain")):
        y = first_row + row * (row_height + row_gap)
        rects[f"{field}_down"] = (controls_left, y, side_width, row_height)
        rects[f"{field}_value"] = (value_x, y, value_width, row_height)
        rects[f"{field}_up"] = (plus_x, y, side_width, row_height)

    bottom_height = max(64, min(76, height // 6))
    bottom_y = height - margin - bottom_height
    gap = max(8, width // 64)
    cancel_width = max(112, int(width * 0.21))
    reset_width = max(104, int(width * 0.19))
    start_x = margin + cancel_width + gap + reset_width + gap
    rects[CANCEL] = (margin, bottom_y, cancel_width, bottom_height)
    rects[RESET] = (margin + cancel_width + gap, bottom_y, reset_width, bottom_height)
    rects[START] = (start_x, bottom_y, width - margin - start_x, bottom_height)
    return rects


def _centered_x(rect: Rect, text: str, scale: float) -> int:
    estimated_width = int(len(text) * 9 * scale)
    return rect[0] + max(8, (rect[2] - estimated_width) // 2)


def _draw_button(canvas, rect: Rect, text: str, color, text_color,
                 scale: float, highlighted: bool) -> None:
    fill = color[1] if highlighted else color[0]
    canvas.draw_rect(*rect, fill, -1)
    text_y = rect[1] + max(6, rect[3] // 2 - int(11 * scale))
    canvas.draw_string(_centered_x(rect, text, scale), text_y, text, text_color, scale)


def draw_screen(screen, image_module, args: argparse.Namespace,
                settings: RecorderSettings, rects: Dict[str, Rect],
                highlighted: Optional[str] = None) -> None:
    width, height = screen.width(), screen.height()
    canvas = image_module.Image(width, height, image_module.Format.FMT_RGB888,
                                bg=image_module.Color.from_rgb(12, 20, 28))
    white = image_module.COLOR_WHITE
    muted = image_module.Color.from_rgb(163, 181, 196)
    panel = image_module.Color.from_rgb(30, 44, 57)
    outline = image_module.Color.from_rgb(76, 97, 113)
    blue = (image_module.Color.from_rgb(36, 105, 164),
            image_module.Color.from_rgb(24, 76, 122))
    gray = (image_module.Color.from_rgb(62, 76, 88),
            image_module.Color.from_rgb(42, 52, 62))
    green = (image_module.Color.from_rgb(17, 157, 83),
             image_module.Color.from_rgb(11, 111, 58))

    canvas.draw_string(18, 12, "DART RECORDER", white, 1.35)
    clip_text = f"CLIP {args.clip}"
    canvas.draw_string(max(300, width - 178), 17, clip_text, muted, 0.95)
    canvas.draw_string(18, 43, "1344x760 | 180 fps | fixed WB", muted, 0.85)

    rows = (
        ("TIME", "seconds", "seconds_down", "seconds_value", "seconds_up"),
        ("EXPOSURE", "exposure_us", "exposure_down", "exposure_value", "exposure_up"),
        ("GAIN", "gain", "gain_down", "gain_value", "gain_up"),
    )
    for label, field, down, value_key, up in rows:
        y = rects[down][1]
        canvas.draw_string(20, y + 13, label, white, 1.12)
        _draw_button(canvas, rects[down], "-", blue, white, 1.45, highlighted == down)
        value_rect = rects[value_key]
        canvas.draw_rect(*value_rect, panel, -1)
        canvas.draw_rect(*value_rect, outline, 2)
        value = settings.display_value(field)
        canvas.draw_string(_centered_x(value_rect, value, 1.1), value_rect[1] + 14,
                           value, white, 1.1)
        _draw_button(canvas, rects[up], "+", blue, white, 1.45, highlighted == up)

    info_y = rects["gain_down"][1] + rects["gain_down"][3] + 12
    canvas.draw_string(20, info_y, "Tap - / + to tune. Saved on START.", muted, 0.85)
    canvas.draw_string(20, info_y + 27, f"Label: {args.label}", muted, 0.85)
    canvas.draw_string(20, info_y + 54, "Screen off during capture; wait for SAVED.", muted, 0.78)

    _draw_button(canvas, rects[CANCEL], "CANCEL", gray, white, 1.0,
                 highlighted == CANCEL)
    _draw_button(canvas, rects[RESET], "DEFAULT", gray, white, 0.9,
                 highlighted == RESET)
    _draw_button(canvas, rects[START], "START RECORDING", green, white, 1.0,
                 highlighted == START)
    screen.show(canvas)


def main() -> int:
    args = parse_args()
    defaults = {"seconds": args.seconds, "exposure_us": args.exposure_us, "gain": args.gain}
    try:
        saved = read_saved_settings(args.state)
        settings = RecorderSettings(defaults, saved)
        from maix import app, display, image, touchscreen

        screen = display.Display()
        touch = touchscreen.TouchScreen()
        print(f"recorder_ui={screen.width()}x{screen.height()} settings_source="
              f"{'saved' if saved is not None else 'defaults'}")
        rects = build_layout(screen.width(), screen.height())
        interactive_rects = {key: value for key, value in rects.items() if not key.endswith("_value")}
        gate = PressReleaseGate(interactive_rects)
        draw_screen(screen, image, args, settings, rects)

        # Discard the touch that launched the app. If it is still held after the
        # guard interval, wait for its release before accepting a new press.
        touch.clear()
        settle_deadline = time.monotonic() + 0.35
        launch_touch_pressed = False
        while time.monotonic() < settle_deadline and not app.need_exit():
            if touch.available(50):
                _, _, launch_touch_pressed = touch.read()
                launch_touch_pressed = bool(launch_touch_pressed)
        while launch_touch_pressed and not app.need_exit():
            if touch.available(100):
                _, _, launch_touch_pressed = touch.read()
                launch_touch_pressed = bool(launch_touch_pressed)

        previous_highlight = None
        while not app.need_exit():
            if not touch.available(100):
                continue
            x, y, pressed = touch.read()
            result = gate.feed(int(x), int(y), bool(pressed), time.monotonic())
            redraw = gate.highlighted != previous_highlight
            previous_highlight = gate.highlighted
            if result in ADJUST_ACTIONS:
                settings.adjust(result)
                print(f"ui_action={result} settings={settings.values}")
                redraw = True
            elif result == RESET:
                settings.reset()
                print(f"ui_action=reset settings={settings.values}")
                redraw = True
            elif result == START:
                write_settings(args.state, settings.values, include_schema=True)
                write_settings(args.output, settings.values, include_schema=False)
                print(f"ui_action=start settings={settings.values}")
                return 0
            elif result == CANCEL:
                print("ui_action=cancel")
                return CANCEL_EXIT_CODE
            if redraw:
                draw_screen(screen, image, args, settings, rects, previous_highlight)
        return CANCEL_EXIT_CODE
    except Exception as exc:
        print(f"record_confirmation_failed={exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
