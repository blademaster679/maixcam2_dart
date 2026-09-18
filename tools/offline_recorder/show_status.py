#!/usr/bin/env python3
"""Best-effort persistent framebuffer status for the offline recorder."""
import sys
import time


def main() -> int:
    title = sys.argv[1] if len(sys.argv) > 1 else "Dart Data Recorder"
    detail = sys.argv[2] if len(sys.argv) > 2 else ""
    state = sys.argv[3] if len(sys.argv) > 3 else "info"
    hold_seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    try:
        from maix import display, image
        screen = display.Display()
        canvas = image.Image(screen.width(), screen.height(), image.Format.FMT_RGB888,
                             bg=image.COLOR_BLACK)
        colors = {
            "recording": image.COLOR_RED,
            "success": image.COLOR_GREEN,
            "warning": image.Color.from_rgb(255, 180, 0),
            "error": image.Color.from_rgb(255, 180, 0),
        }
        color = colors.get(state, image.COLOR_WHITE)
        canvas.draw_string(20, 30, title, color, 1.6)
        canvas.draw_string(20, 85, detail, image.COLOR_WHITE, 1.2)
        screen.show(canvas)
        if hold_seconds > 0:
            time.sleep(hold_seconds)
    except Exception as exc:  # The recording remains usable without display feedback.
        print(f"display_status_failed={exc}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
