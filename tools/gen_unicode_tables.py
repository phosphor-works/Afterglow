#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Generates libs/afterglow-vt/src/UnicodeTables.inc from the Unicode Character
# Database. Run it whenever the UCD version is bumped:
#
#   python3 -I tools/gen_unicode_tables.py <ucd-dir> libs/afterglow-vt/src/UnicodeTables.inc
#
# <ucd-dir> must hold these files from https://www.unicode.org/Public/<ver>/ucd/,
# flattened into one directory:
#   EastAsianWidth.txt, DerivedCoreProperties.txt, GraphemeBreakProperty.txt,
#   emoji-data.txt (from ucd/emoji/), DerivedGeneralCategory.txt (from ucd/extracted/)
#
# Output: a two-stage lookup. stage1[cp >> 8] picks a 256-entry block, the
# block entry indexes into a table of unique packed property words. The packed
# word layout must match its decoder in libs/afterglow-vt/src/Unicode.cpp.

import re
import sys
from pathlib import Path

MAX_CP = 0x110000

GCB = ["Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
       "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT"]
INCB = ["None", "Linker", "Consonant", "Extend"]

LINE = re.compile(r"^([0-9A-F]+)(?:\.\.([0-9A-F]+))?\s*;\s*([^#;]+?)\s*(?:;\s*([^#]+?)\s*)?(?:#.*)?$")
MISSING = re.compile(r"^#\s*@missing:\s*([0-9A-F]+)\.\.([0-9A-F]+)\s*;\s*(\S+)")


def ranges(path, with_missing=False):
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        if with_missing:
            m = MISSING.match(raw)
            if m:
                yield int(m.group(1), 16), int(m.group(2), 16), m.group(3), None, True
                continue
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = LINE.match(line)
        if not m:
            continue
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        yield lo, hi, m.group(3).strip(), (m.group(4) or "").strip() or None, False


def version_of(path):
    first = Path(path).read_text(encoding="utf-8").splitlines()[0]
    m = re.search(r"-(\d+\.\d+\.\d+)\.txt", first)
    return m.group(1) if m else "unknown"


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: gen_unicode_tables.py <ucd-dir> <output.inc>")
    ucd = Path(sys.argv[1])
    out = Path(sys.argv[2])

    gcb = [0] * MAX_CP
    for lo, hi, val, _, _ in ranges(ucd / "GraphemeBreakProperty.txt"):
        idx = GCB.index(val)
        for cp in range(lo, hi + 1):
            gcb[cp] = idx

    incb = [0] * MAX_CP
    for lo, hi, prop, val, _ in ranges(ucd / "DerivedCoreProperties.txt"):
        if prop == "InCB" and val:
            idx = INCB.index(val)
            for cp in range(lo, hi + 1):
                incb[cp] = idx

    ext_pict = [False] * MAX_CP
    emoji = [False] * MAX_CP
    emoji_pres = [False] * MAX_CP
    for lo, hi, prop, _, _ in ranges(ucd / "emoji-data.txt"):
        target = {"Extended_Pictographic": ext_pict, "Emoji": emoji,
                  "Emoji_Presentation": emoji_pres}.get(prop)
        if target is None:
            continue
        for cp in range(lo, hi + 1):
            target[cp] = True

    # East Asian Width: @missing defaults first, explicit entries override.
    eaw = ["N"] * MAX_CP
    explicit = []
    for lo, hi, val, _, missing in ranges(ucd / "EastAsianWidth.txt", with_missing=True):
        if missing:
            for cp in range(lo, hi + 1):
                eaw[cp] = val
        else:
            explicit.append((lo, hi, val))
    for lo, hi, val in explicit:
        for cp in range(lo, hi + 1):
            eaw[cp] = val

    gc = ["Cn"] * MAX_CP
    for lo, hi, val, _, _ in ranges(ucd / "DerivedGeneralCategory.txt"):
        for cp in range(lo, hi + 1):
            gc[cp] = val

    def width(cp):
        if cp == 0x00AD:  # soft hyphen renders as a hyphen in terminals
            return 1
        if gc[cp] in ("Mn", "Me", "Cf", "Cc"):
            return 0
        if 0x1160 <= cp <= 0x11FF or 0xD7B0 <= cp <= 0xD7FF:  # Hangul medial vowels / final consonants
            return 0
        if eaw[cp] in ("W", "F") or emoji_pres[cp]:
            return 2
        return 1

    packed = [0] * MAX_CP
    for cp in range(MAX_CP):
        if 0xD800 <= cp <= 0xDFFF:
            w = 0
        else:
            w = width(cp)
        vs16 = 1 if (emoji[cp] and not emoji_pres[cp]) else 0
        # East Asian Ambiguous: one cell normally, two where CJK text expects it.
        ambiguous = 1 if (eaw[cp] == "A" and w == 1) else 0
        packed[cp] = (gcb[cp]
                      | (int(ext_pict[cp]) << 4)
                      | (incb[cp] << 5)
                      | (w << 7)
                      | (vs16 << 9)
                      | (ambiguous << 10))

    values = sorted(set(packed))
    value_index = {v: i for i, v in enumerate(values)}
    if len(values) > 256:
        raise ValueError(f"Property table needs {len(values)} values, exceeding the 256-entry index capacity")

    blocks = []
    block_index = {}
    stage1 = []
    for base in range(0, MAX_CP, 256):
        blk = tuple(value_index[packed[cp]] for cp in range(base, base + 256))
        if blk not in block_index:
            block_index[blk] = len(blocks)
            blocks.append(blk)
        stage1.append(block_index[blk])

    ver = version_of(ucd / "EastAsianWidth.txt")
    lines = [
        "// SPDX-FileCopyrightText: 2026 fuddlesworth",
        "// SPDX-License-Identifier: LGPL-2.1-or-later",
        f"// GENERATED by tools/gen_unicode_tables.py from UCD {ver}. Do not edit.",
        "// clang-format off",
        "",
        f'inline constexpr const char UcdVersion[] = "{ver}";',
        "",
        f"inline constexpr std::uint16_t PropValues[{len(values)}] = {{",
    ]
    for i in range(0, len(values), 12):
        lines.append("    " + ", ".join(f"0x{v:04x}" for v in values[i:i + 12]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr std::uint16_t Stage1[{len(stage1)}] = {{")
    for i in range(0, len(stage1), 16):
        lines.append("    " + ", ".join(str(v) for v in stage1[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr std::uint8_t Stage2[{len(blocks) * 256}] = {{")
    for blk in blocks:
        for i in range(0, 256, 32):
            lines.append("    " + ",".join(str(v) for v in blk[i:i + 32]) + ",")
    lines.append("};")
    lines.append("// clang-format on")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"UCD {ver}: {len(values)} property values, {len(blocks)} blocks, "
          f"{len(stage1) * 2 + len(blocks) * 256} bytes")


if __name__ == "__main__":
    main()
