"""Game manuals for the ROM list: teletext style pages of 32x25 characters.

Source (a .txt next to the ROMs, named in roms/settings.csv), phpBB style tags:

    [PAGESTART=1]                 a page; the numbers should count up from 1
    [color=red]Story              colour from here on, until changed; each page starts in white
    [PAGEEND]
    \\[ \\] \\\\                     a literal [ ] \\

A line that holds only tags gives no line on the screen; an empty line is an empty line. Lines longer than 32 characters and
pages longer than 25 lines are cut, with a warning. Colours: black red green yellow blue magenta
cyan white.

Device format, a cut-down VT100: '\\n' ends a line, '\\f' starts the next page, ESC followed by
'0'-'7' sets the colour (ANSI order, as in the list above), everything else is a character of the
font's code page 437 (so umlauts work).

Preview in a terminal:  python3 tools/nesmanual.py roms/zelda1.txt
"""

import sys

COLS, ROWS = 32, 25
COLOURS = ["black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"]
WHITE = 7


def convert(path, label=None, warn=print):
    """Returns (device bytes, number of pages)."""
    label = label or path

    def problem(lineNo, msg):
        warn("%s line %d: %s" % (label, lineNo, msg))

    pages = []            # finished pages, lists of output lines (bytes)
    page = None           # the page being built, or None outside a page
    colour = WHITE
    pending = b""         # colour codes from tag-only lines, put before the next text line

    def endPage():
        nonlocal page, pending
        pages.append(page)
        page, pending = None, b""

    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read().replace("\r\n", "\n").replace("\r", "\n")

    for lineNo, line in enumerate(text.split("\n"), 1):
        out = bytearray()     # this line's bytes so far
        visible = 0
        hasText = hasTag = cut = False
        i = 0
        while i < len(line):
            ch = line[i]
            if ch == "\\" and i + 1 < len(line):
                ch = line[i + 1]
                i += 2
            elif ch == "[" and "]" in line[i:]:
                end = line.index("]", i)
                tag = line[i + 1:end]
                name, _, value = tag.partition("=")
                name, value = name.strip().lower(), value.strip()
                i = end + 1
                hasTag = True
                if name == "pagestart":
                    if page is not None:
                        problem(lineNo, "PAGESTART inside a page, the page before ends here")
                        if hasText:
                            page.append(pending + bytes(out))
                        endPage()
                        out, hasText = bytearray(), False
                    if value and value != str(len(pages) + 1):
                        problem(lineNo, "PAGESTART=%s, but this is page %d" % (value, len(pages) + 1))
                    page, colour = [], WHITE
                elif name == "pageend":
                    if page is None:
                        problem(lineNo, "PAGEEND outside a page")
                    else:
                        if hasText:
                            page.append(pending + bytes(out))
                        endPage()
                        out, hasText = bytearray(), False
                elif name in ("color", "colour"):
                    if value.lower() not in COLOURS:
                        problem(lineNo, "unknown colour '%s' (%s)" % (value, ", ".join(COLOURS)))
                    elif page is not None:
                        c = COLOURS.index(value.lower())
                        if c != colour:
                            out += b"\x1b" + str(c).encode()
                            colour = c
                else:
                    problem(lineNo, "unknown tag [%s], ignored" % tag)
                continue
            else:
                if ch == "[":
                    problem(lineNo, "'[' without ']', taken as text (write \\[ for a bracket)")
                i += 1

            # an ordinary character
            if page is None:
                if not ch.isspace():
                    problem(lineNo, "text outside a page, ignored")
                    break
                continue
            hasText = True
            if visible >= COLS:
                cut = True
                continue
            out += ch.encode("cp437", errors="replace")
            visible += 1

        if page is None:
            continue
        if cut:
            problem(lineNo, "longer than %d characters, cut" % COLS)
        if hasText or not hasTag:
            # a text line, or an empty one
            page.append(pending + bytes(out).rstrip(b" "))
            pending = b""
        else:
            pending += bytes(out)     # only tags: colour changes apply from the next line on

    if page is not None:
        warn("%s: the last page has no PAGEEND" % label)
        endPage()

    result = []
    for n, lines in enumerate(pages, 1):
        while lines and lines[-1] == b"":
            lines = lines[:-1]            # trailing empty lines
        if len(lines) > ROWS:
            warn("%s: page %d has %d lines, cut to %d" % (label, n, len(lines), ROWS))
            lines = lines[:ROWS]
        result.append(b"\n".join(lines))
    return b"\f".join(result), len(result)


def preview(data):
    """The device format shown in a terminal, with ANSI colours."""
    for n, page in enumerate(data.split(b"\f"), 1):
        print("\x1b[0m----- page %d " % n + "-" * 18)
        out, i = "\x1b[37m", 0
        while i < len(page):
            b = page[i]
            if b == 0x1b:
                out += "\x1b[%dm" % (30 + page[i + 1] - 48)
                i += 2
                continue
            out += bytes([b]).decode("cp437")
            i += 1
        print(out + "\x1b[0m")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: nesmanual.py manual.txt")
    data, pages = convert(sys.argv[1], warn=lambda m: print("WARNING: " + m))
    preview(data)
    print("%d pages, %d bytes" % (pages, len(data)))
