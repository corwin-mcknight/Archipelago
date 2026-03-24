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


def bold(text: str) -> str:
    return ansi(text, 1)


def green(text: str) -> str:
    return ansi(text, 32)


def red(text: str) -> str:
    return ansi(text, 31)


def cyan(text: str) -> str:
    return ansi(text, 36)


def yellow(text: str) -> str:
    return ansi(text, 33)


def dim(text: str) -> str:
    return ansi(text, 2)


def fmt_timing_table(timings: list[tuple[str, float]], total: float) -> str:
    """Format a build timing summary table.

    timings is a list of (package_name, elapsed_seconds).
    """
    if not timings:
        return ""

    name_width = max(len(name) for name, _ in timings)
    name_width = max(name_width, 5)  # minimum width

    lines = [bold("Build timing:")]
    for name, elapsed in timings:
        lines.append(f"  {name:<{name_width}}  {fmt_duration(elapsed):>8}")
    lines.append(f"  {'─' * (name_width + 10)}")
    lines.append(f"  {'Total':<{name_width}}  {bold(fmt_duration(total)):>8}")
    return "\n".join(lines)


def fmt_duration(seconds: float) -> str:
    """Format elapsed seconds as a human-readable string."""
    if seconds < 60:
        return f"{seconds:.1f}s"
    m = int(seconds // 60)
    s = seconds % 60
    return f"{m}m{s:.0f}s"
