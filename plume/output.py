"""ANSI output formatting helpers for Plume."""

import sys


def _use_color() -> bool:
    return hasattr(sys.stdout, "isatty") and sys.stdout.isatty()


def ansi(text: str, *codes: int) -> str:
    """Wrap text in ANSI escape codes, or return plain text if not a TTY."""
    if not _use_color():
        return text
    code_str = ";".join(str(c) for c in codes)
    return f"\033[{code_str}m{text}\033[0m"


def _make_style(code: int):
    return lambda text: ansi(text, code)

bold, green, red, cyan, yellow, dim = (
    _make_style(c) for c in (1, 32, 31, 36, 33, 2)
)


def fmt_duration(seconds: float) -> str:
    """Format elapsed seconds as a human-readable string."""
    if seconds < 60:
        return f"{seconds:.1f}s"
    m = int(seconds // 60)
    s = seconds % 60
    return f"{m}m{s:.0f}s"


# Reasons that add no information next to the surrounding output.
_QUIET_REASONS = ("never built", "not installed")


def fmt_reason(reason: str | None) -> str:
    """Format a build/install reason as a dim suffix, or '' for uninformative ones."""
    if not reason or reason in _QUIET_REASONS:
        return ""
    return f"  {dim(f'({reason})')}"


_line_open = False


def open_line(text: str):
    """Start a status line, leaving it open for close_line to complete."""
    global _line_open
    print(f"{text} ", end="", flush=True)
    _line_open = True


def close_line(text: str = ""):
    """Complete the currently open status line."""
    global _line_open
    print(text, flush=True)
    _line_open = False


def break_line():
    """Terminate any open status line before out-of-band output; no-op otherwise."""
    global _line_open
    if _line_open:
        print(flush=True)
        _line_open = False
