# PlatformIO pre-build script: builds the games in roms/ into the firmware.
#
# roms/settings.csv lists the games in menu order, one per line:
#   "file name.nes", "name shown in the menu", "picture.png", "manual.txt", options
# Lines starting with # are comments. Options are "key=value" pairs separated by ';' (see
# RomConfig in src/main.cpp: save, region, overclock). A ROM in roms/
# that the CSV does not list is still built in, after the listed ones, named after its file.
#
# Writes into src/generated/:
#   roms.S         .incbin's each ROM into flash rodata
#   romindex.cpp   the table the ROM list reads (see src/romindex.h), with the pictures
#   pics/          converted pictures, a cache so unchanged PNGs are not converted again
# Constant data in flash is mapped into the address space through the cache, so the emulator reads
# the ROMs in place: no RAM, no copying, no flash writes. Files are only rewritten when they
# change, so an unchanged set does not trigger a rebuild.
#
# Pictures: a PNG is scaled to fit 64x64 in NES palette colours (tools/nesimage.py): pixel art
# keeps its own colours, photos are dithered.
# A missing picture is a warning; the menu then shows an empty square.
#
# Manuals: teletext style pages of 32x25 characters with phpBB style tags, converted to a cut-down
# VT100 form for the device by tools/nesmanual.py (the format is described there). A missing
# manual is a warning; the menu then has none to show.
#
# An optional "<rom name>.cfg" next to a ROM (same name, .cfg instead of .nes) holds the same
# settings, one "key = value" per line; it is merged after the CSV's options.

import csv
import os
import sys

Import("env")

project = env.subst("$PROJECT_DIR")
romDir = os.path.join(project, "roms")
outDir = os.path.join(project, "src", "generated")
picDir = os.path.join(outDir, "pics")
paletteH = os.path.join(project, "src", "nes", "palette.h")

sys.path.insert(0, os.path.join(project, "tools"))
import nesimage
import nesmanual


def warn(msg):
    print("embed_roms: WARNING: " + msg)


def cString(s):
    # a C string literal of the UTF-8 bytes; escapes are closed with "" so a following hex digit
    # is not read as part of them
    out = ""
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch == "\\" or ch == '"':
            out += "\\" + ch
        elif ch == "\n":
            out += "\\n"
        elif b < 32 or b > 126:
            out += '\\x%02x""' % b
        else:
            out += ch
    return '"' + out + '"'


def cBytes(data):
    # a C string literal of raw bytes
    out = ""
    for b in data:
        ch = chr(b)
        if ch == "\\" or ch == '"':
            out += "\\" + ch
        elif ch == "\n":
            out += "\\n"
        elif ch == "\f":
            out += "\\f"
        elif b < 32 or b > 126:
            out += '\\x%02x""' % b
        else:
            out += ch
    return '"' + out + '"'


def asmString(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def writeIfChanged(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    old = None
    if os.path.exists(path):
        with open(path, "rb" if mode == "wb" else "r") as f:
            old = f.read()
    if old != data:
        with open(path, mode) as f:
            f.write(data)


# --- the game list ---

files = sorted((f for f in os.listdir(romDir) if f.lower().endswith(".nes")) if os.path.isdir(romDir) else [],
               key=str.lower)
games = []           # dicts: file, name, picture, manual, options
listed = set()

csvPath = os.path.join(romDir, "settings.csv")
if os.path.exists(csvPath):
    with open(csvPath, newline="", encoding="utf-8") as f:
        for lineNo, row in enumerate(csv.reader(f, skipinitialspace=True), 1):
            if not row or not "".join(row).strip() or row[0].lstrip().startswith("#"):
                continue
            row = [field.strip() for field in row]
            if len(row) < 2 or not row[0].lower().endswith(".nes") or any('"' in field for field in row):
                warn("settings.csv line %d looks malformed (a missing quote?): %s" % (lineNo, row))
                continue
            row += [""] * (5 - len(row))
            if row[0] not in files:
                warn("settings.csv line %d: %s is not in roms/, skipped" % (lineNo, row[0]))
                continue
            if row[0] in listed:
                warn("settings.csv line %d: %s is listed twice, skipped" % (lineNo, row[0]))
                continue
            listed.add(row[0])
            games.append({"file": row[0], "name": row[1] or row[0][:-4], "picture": row[2],
                          "manual": row[3], "options": row[4]})
else:
    warn("no roms/settings.csv, listing the ROMs by file name")

for name in files:
    if name not in listed:
        if os.path.exists(csvPath):
            warn("%s is not in settings.csv, listed at the end under its file name" % name)
        games.append({"file": name, "name": name[:-4], "picture": "", "manual": "", "options": ""})


# --- pictures, cached in src/generated/pics ---

os.makedirs(picDir, exist_ok=True)
palette = None

def picture(game):
    global palette
    if not game["picture"]:
        warn("%s: no picture given, the menu shows an empty square" % game["file"])
        return None
    png = os.path.join(romDir, game["picture"])
    if not os.path.exists(png):
        warn("%s: picture %s not found, the menu shows an empty square" % (game["file"], game["picture"]))
        return None
    cache = os.path.join(picDir, game["picture"] + ".bin")
    sources = [png, paletteH, os.path.join(project, "tools", "nesimage.py")]
    if os.path.exists(cache) and os.path.getmtime(cache) >= max(os.path.getmtime(f) for f in sources):
        with open(cache, "rb") as f:
            return f.read()
    try:
        if palette is None:
            palette = nesimage.load_palette(paletteH)
        data, used = nesimage.convert(png, palette)
    except Exception as e:
        warn("%s: cannot convert %s (%s), the menu shows an empty square" % (game["file"], game["picture"], e))
        return None
    with open(cache, "wb") as f:
        f.write(data)
    print("embed_roms: converted %s (as %s)" % (game["picture"], used))
    return data


# --- generated sources ---

asm = ["// generated by tools/embed_roms.py from roms/, do not edit", ""]
cpp = ["// generated by tools/embed_roms.py from roms/, do not edit", "",
       '#include "../romindex.h"', ""]
entries = []
total = 0
for i, g in enumerate(games):
    path = os.path.join(romDir, g["file"])
    size = os.path.getsize(path)
    total += size
    asm += ['.section .rodata.nesrom%d, "a"' % i, ".balign 4",
            ".global nesrom_%d" % i, "nesrom_%d:" % i, ".incbin " + asmString(path), ""]
    cpp.append("extern const uint8_t nesrom_%d[];" % i)

    pic = picture(g)
    picRef = "nullptr"
    if pic:
        cpp.append("static const uint8_t picture_%d[%d] = {" % (i, len(pic)))
        for k in range(0, len(pic), 32):
            cpp.append("  " + ",".join(str(b) for b in pic[k:k + 32]) + ",")
        cpp.append("};")
        picRef = "picture_%d" % i

    manualRef = "nullptr"
    if g["manual"]:
        mpath = os.path.join(romDir, g["manual"])
        if os.path.exists(mpath):
            data, pages = nesmanual.convert(mpath, g["manual"], warn)
            if pages:
                manualRef = cBytes(data)
            else:
                warn("%s: manual %s has no pages" % (g["file"], g["manual"]))
        else:
            warn("%s: manual %s not found" % (g["file"], g["manual"]))

    # settings: the CSV's options column ("key=value;key=value") and an optional <rom>.cfg,
    # merged into one "key = value" per line text; the .cfg comes last and so wins
    settings = "\n".join(part.strip() for part in g["options"].split(";") if part.strip())
    cfgPath = os.path.join(romDir, g["file"][:-4] + ".cfg")
    if os.path.exists(cfgPath):
        with open(cfgPath, encoding="utf-8", errors="replace") as f:
            settings += "\n" + f.read()
    cfg = cString(settings) if settings.strip() else "nullptr"

    entries.append("  { %s, %s, nesrom_%d, %d, %s, %s, %s }," % (cString(g["file"]), cString(g["name"]), i, size, picRef, manualRef, cfg))

cpp += ["", "const RomBlob g_roms[] = {"] + (entries or ["  { nullptr, nullptr, nullptr, 0, nullptr, nullptr, nullptr },"])
cpp += ["};", "", "const int g_romCount = %d;" % len(games), ""]

writeIfChanged(os.path.join(outDir, "roms.S"), "\n".join(asm) + "\n")
writeIfChanged(os.path.join(outDir, "romindex.cpp"), "\n".join(cpp))
print("embed_roms: %d games, %d KB of ROMs" % (len(games), total // 1024))
