#!/usr/bin/env python3
"""
termshot.py - run a terminal program under a pty and save the screen as SVG.

Used to produce the screenshots in docs/screenshots/ from the real programs
rather than from mock-ups: it drives vwsd or vwsctl in a pty of a fixed size,
optionally fires shell commands partway through (to inject a fault while the
dashboard is up), then renders pyte's final screen buffer - characters plus
per-cell colour attributes - into a standalone SVG.

SVG rather than PNG so the result is small, scales, and keeps its text
selectable and searchable.

  usage: termshot.py --out FILE.svg [--cols N] [--rows N] [--seconds S]
                     [--at SECONDS:SHELL_COMMAND ...] [--keys SECONDS:KEYS ...]
                     [--title TEXT] -- PROGRAM [ARGS...]
"""
import argparse
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import xml.sax.saxutils as sx

import pyte

# A dark palette close to a default terminal. pyte reports the eight ANSI
# names, and calls yellow "brown".
PALETTE = {
    "black": "#2b2b35", "red": "#e5534b", "green": "#4cc46a",
    "brown": "#d4a13a", "yellow": "#d4a13a", "blue": "#539bf5",
    "magenta": "#b083f0", "cyan": "#39c5cf", "white": "#d9dee3",
    "brightblack": "#6a6a78", "brightred": "#ff6b63",
    "brightgreen": "#6bdd86", "brightyellow": "#eebc5a",
    "brightblue": "#6cb0ff", "brightmagenta": "#c79bff",
    "brightcyan": "#56d9e3", "brightwhite": "#ffffff",
}
FG_DEFAULT = "#d9dee3"
BG_DEFAULT = "#14141b"

CELL_W, CELL_H, PAD = 8.0, 17.0, 14.0
FONT = ("'DejaVu Sans Mono','JetBrains Mono','Liberation Mono'," "'Courier New',monospace")


def colour(name, fallback):
    if name in (None, "default"):
        return fallback
    if name in PALETTE:
        return PALETTE[name]
    if len(name) == 6:                      # pyte gives 256-colour as raw hex
        try:
            int(name, 16)
            return "#" + name
        except ValueError:
            pass
    return fallback


def run(args):
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ)
        env.update(TERM="xterm-256color", LANG="C.utf8", LC_ALL="C.utf8",
                   COLUMNS=str(args.cols), LINES=str(args.rows))
        os.execvpe(args.program[0], args.program, env)

    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", args.rows, args.cols, 0, 0))

    screen = pyte.Screen(args.cols, args.rows)
    stream = pyte.Stream(screen)

    # Scheduled side effects: shell commands and keystrokes, soonest first.
    events = []
    for spec in args.at or []:
        when, _, cmd = spec.partition(":")
        events.append((float(when), "sh", cmd))
    for spec in args.keys or []:
        when, _, keys = spec.partition(":")
        events.append((float(when), "key", keys))
    events.sort(key=lambda e: e[0])

    start = time.time()
    while True:
        now = time.time() - start
        if now >= args.seconds:
            break

        while events and events[0][0] <= now:
            _, kind, payload = events.pop(0)
            if kind == "sh":
                subprocess.run(payload, shell=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            else:
                os.write(fd, payload.encode())

        timeout = 0.1
        if events:
            timeout = max(0.01, min(timeout, events[0][0] - now))

        r, _, _ = select.select([fd], [], [], timeout)
        if r:
            try:
                data = os.read(fd, 1 << 16)
            except OSError:
                break
            if not data:
                break
            stream.feed(data.decode("utf-8", "replace"))

    os.kill(pid, signal.SIGTERM)
    time.sleep(0.3)
    try:
        os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        pass
    os.close(fd)
    return screen


def to_svg(screen, args):
    rows = [r for r in range(args.rows)]
    # Trim trailing blank lines so the image is no taller than the content.
    while rows and not screen.display[rows[-1]].strip():
        rows.pop()
    if not rows:
        rows = [0]
    nrows = len(rows)

    head = PAD + (22.0 if args.title else 0.0)
    width = PAD * 2 + args.cols * CELL_W
    height = head + nrows * CELL_H + PAD

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" '
        f'height="{height:.0f}" viewBox="0 0 {width:.0f} {height:.0f}" '
        f'font-family={sx.quoteattr(FONT)} font-size="12.5">',
        f'<rect width="100%" height="100%" rx="7" fill="{BG_DEFAULT}"/>',
    ]
    if args.title:
        out.append(
            f'<text x="{PAD:.0f}" y="{PAD + 7:.0f}" fill="#8b949e" '
            f'font-size="11.5">{sx.escape(args.title)}</text>')

    for ri, y in enumerate(rows):
        line = screen.buffer[y]
        ytop = head + ri * CELL_H
        base = ytop + CELL_H - 4.5

        # Emit background runs first, then one text span per attribute run,
        # so the SVG stays compact instead of one element per character.
        runs, cur = [], None
        for x in range(args.cols):
            ch = line[x]
            data = ch.data if ch.data and ch.data != "\x00" else " "
            fg = colour(ch.fg, FG_DEFAULT)
            bg = colour(ch.bg, BG_DEFAULT)
            if ch.reverse:
                fg, bg = bg, fg
            if ch.bold and ch.fg == "default":
                fg = "#ffffff"
            key = (fg, bg, bool(ch.bold), bool(ch.underscore))
            if cur and cur[0] == key and cur[2] == x:
                cur[1].append(data)
                cur[2] = x + 1
            else:
                cur = [key, [data], x + 1, x]
                runs.append(cur)

        for (fg, bg, bold, under), chars, end, x0 in runs:
            text = "".join(chars)
            if bg != BG_DEFAULT:
                out.append(
                    f'<rect x="{PAD + x0 * CELL_W:.1f}" y="{ytop:.1f}" '
                    f'width="{(end - x0) * CELL_W:.1f}" height="{CELL_H:.1f}" '
                    f'fill="{bg}"/>')
            if not text.strip():
                continue
            attrs = f' fill="{fg}"'
            if bold:
                attrs += ' font-weight="bold"'
            if under:
                attrs += ' text-decoration="underline"'
            out.append(
                f'<text x="{PAD + x0 * CELL_W:.1f}" y="{base:.1f}"'
                f'{attrs} xml:space="preserve">{sx.escape(text)}</text>')

    out.append("</svg>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--cols", type=int, default=118)
    ap.add_argument("--rows", type=int, default=32)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--at", action="append", metavar="SEC:CMD")
    ap.add_argument("--keys", action="append", metavar="SEC:KEYS")
    ap.add_argument("--title", default="")
    ap.add_argument("program", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    if args.program and args.program[0] == "--":
        args.program = args.program[1:]
    if not args.program:
        ap.error("no program given")

    screen = run(args)
    with open(args.out, "w") as fh:
        fh.write(to_svg(screen, args))

    text = args.out.rsplit(".", 1)[0] + ".txt"
    with open(text, "w") as fh:
        for line in screen.display:
            fh.write(line.rstrip() + "\n")

    print(f"wrote {args.out} and {text}")


if __name__ == "__main__":
    main()
