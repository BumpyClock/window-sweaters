"""Flat preview of catalogue charts for quick design iteration.

Renders each theme as a labelled strip of repeated chart cells on its base yarn.
This is a design aid only: it ignores knit texture, corners, cuffs and fitted
repeats. Use the Knitting tests (tests/Knitting/outputs) for production renders.

    python scripts/preview-designs.py                     # Windows-authored themes
    python scripts/preview-designs.py --all               # every catalogue theme
    python scripts/preview-designs.py xbox steam -o x.png # selected theme ids
"""
import argparse
import pathlib
import re
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
DESIGNS = ROOT / 'src' / 'Designs'
HEX = r'0x([0-9a-fA-F]{8})u'

# 3x5 glyphs, rows top-down, for strip labels.
FONT = dict(zip('abcdefghijklmnopqrstuvwxyz0123456789-', (
    '010101111101101', '110101110101110', '011100100100011', '110101101101110', '111100110100111',
    '111100110100100', '011100101101011', '101101111101101', '111010010010111', '001001001101010',
    '101101110101101', '100100100100111', '101111111101101', '110101101101101', '010101101101010',
    '110101110100100', '010101101111011', '110101110101101', '011100010001110', '111010010010010',
    '101101101101111', '101101101101010', '101101111111101', '101101010101101', '101101010010010',
    '111001010100111', '010101101101010', '010110010010111', '110001010100111', '110001010001110',
    '101101111001001', '111100110001110', '011100110101010', '111001010010010', '010101010101010',
    '010101011001110', '000000111000000')))


def load_catalogue():
    """Return (charts, themes): charts maps name -> (rows, yarn); themes is [(id, chart, base, windows)]."""
    charts = {}
    authored = (DESIGNS / 'AuthoredCharts.inc').read_text(encoding='utf-8')
    for m in re.finditer(r'\{ "([\w-]+)", \{(.*?)\},\s*\{(.*?)\} \}', authored, re.S):
        charts[m[1]] = (re.findall(r'"([.a-f]+)"', m[2]), [int(x, 16) for x in re.findall(HEX, m[3])])
    windows = (DESIGNS / 'WindowsDesigns.inc').read_text(encoding='utf-8')
    rows = {m[1]: re.findall(r'"([.a-f]+)"', m[2])
            for m in re.finditer(r'k_(\w+)_rows\[\] = \{(.*?)\};', windows, re.S)}
    yarn = {m[1]: [int(x, 16) for x in re.findall(HEX, m[2])]
            for m in re.finditer(r'k_(\w+)_yarn\[6\] = \{(.*?)\};', windows, re.S)}
    for m in re.finditer(r'WINDOWS_CHART\("([\w-]+)", (\d+), k_(\w+)_rows', windows):
        name, width, key = m[1], int(m[2]), m[3]
        bad = [r for r in rows[key] if len(r) != width]
        if bad:
            sys.exit(f'{name}: declared width {width}, rows of width {sorted({len(r) for r in bad})}')
        charts[name] = (rows[key], yarn[key])
    theme = r'\{ "([\w-]+)", "[^"]*", (NULL|"[^"]*"), "([\w-]+)", ' + HEX
    themes = [(m[1], m[3], int(m[4], 16), False)
              for m in re.finditer(theme, (DESIGNS / 'ThemeViews.inc').read_text(encoding='utf-8'))]
    themes += [(m[1], m[3], int(m[4], 16), True) for m in re.finditer(theme, windows)]
    return charts, themes


def rgb(argb):
    return (argb >> 16) & 255, (argb >> 8) & 255, argb & 255


def render(entries, path, cells=96, px=7):
    """entries: [(label, rows, yarn, base_argb)]. Writes an RGB PNG."""
    width = cells * px
    canvas = []
    paper, ink = (250, 248, 243), (60, 60, 60)
    for label, rows, yarn, base in entries:
        unused = sorted({ch for row in rows for ch in row if ch != '.' and ord(ch) - 97 >= len(yarn)})
        if unused:
            sys.exit(f'{label}: cells {", ".join(unused)} have no yarn')
        for y in range(5):  # label line
            line = [paper] * width
            for i, ch in enumerate(label.lower()[:width // 8]):
                glyph = FONT.get(ch, '0' * 15)
                for x in range(3):
                    if glyph[y * 3 + x] == '1':
                        for dx in range(2):
                            line[4 + i * 8 + x * 2 + dx] = ink
            canvas += [line, line]
        canvas += [[paper] * width] * 3
        for row in rows:
            line = []
            for x in range(cells):
                ch = row[x % len(row)]
                colour = rgb(base if ch == '.' else yarn[ord(ch) - 97])
                line += [colour] * px
            canvas += [line] * px
        canvas += [[paper] * width] * 10
    raw = b''.join(b'\0' + bytes(c for p in line for c in p) for line in canvas)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    pathlib.Path(path).write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, len(canvas), 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('themes', nargs='*', help='theme ids to render (default: Windows-authored themes)')
    parser.add_argument('--all', action='store_true', help='render every catalogue theme')
    parser.add_argument('-o', '--out', default=ROOT / 'out' / 'design-preview.png', help='output PNG path')
    parser.add_argument('--cells', type=int, default=96, help='stitches per strip')
    parser.add_argument('--px', type=int, default=7, help='pixels per stitch')
    args = parser.parse_args()
    charts, themes = load_catalogue()
    if args.themes:
        known = {t[0] for t in themes}
        missing = [t for t in args.themes if t not in known]
        if missing:
            sys.exit(f'unknown theme ids: {", ".join(missing)}')
        themes = [t for t in themes if t[0] in args.themes]
    elif not args.all:
        themes = [t for t in themes if t[3]]
    entries = [(tid, *charts[chart], base) for tid, chart, base, _ in themes]
    pathlib.Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    render(entries, args.out, args.cells, args.px)
    print(f'{args.out}: {len(entries)} themes')


if __name__ == '__main__':
    main()
