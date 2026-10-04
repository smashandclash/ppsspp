#!/usr/bin/env python3
"""Builds the client's art and fonts for one platform, from the real thing:

  * the 51-card deck (names, values, art) from the Smash&Clash API and site,
  * the card back and the Smash&Clash logo from smashandclash.in,
  * the game's typefaces (Luckiest Guy: Apache 2.0; Lilita One, Rubik, the Noto
    symbol fonts and the GBA's pixel fonts Jersey, Bitcount and Tiny5: SIL Open Font
    License), from Google Fonts.

Usage:  python3 tools/make_assets.py ds|psp|gba|host|host-gba OUT_DIR
Needs:  Python 3.9+, Pillow (with WebP) and fontTools:  pip install pillow fonttools

Writes OUT_DIR/assets.h, OUT_DIR/assets_data.c and the big binaries (card art) that
OUT_DIR/assets_bin.S pulls in with .incbin. Downloads are cached in tools/cache/.
"""
import io
import json
import os
import struct
import sys
import urllib.parse
import urllib.request

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

SITE = "https://www.smashandclash.in"
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache")

FONTS = {
    "display": ("LuckiestGuy-Regular.ttf", "https://github.com/google/fonts/raw/main/apache/luckiestguy/LuckiestGuy-Regular.ttf"),
    "label": ("LilitaOne-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/lilitaone/LilitaOne-Regular.ttf"),
    "body": ("Rubik-wght.ttf", "https://github.com/google/fonts/raw/main/ofl/rubik/Rubik%5Bwght%5D.ttf"),
    "fallback": ("NotoSans-wdth-wght.ttf", "https://github.com/google/fonts/raw/main/ofl/notosans/NotoSans%5Bwdth%2Cwght%5D.ttf"),
    "symbols": ("NotoSansSymbols2-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/notosanssymbols2/NotoSansSymbols2-Regular.ttf"),
    "math": ("NotoSansMath-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/notosansmath/NotoSansMath-Regular.ttf"),  # the arrows in move names
    # pixel fonts for the GBA (all SIL Open Font License): drawn on their own pixel grid, no smoothing
    "jersey10": ("Jersey10-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/jersey10/Jersey10-Regular.ttf"),
    "jersey15": ("Jersey15-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/jersey15/Jersey15-Regular.ttf"),
    "jersey20": ("Jersey20-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/jersey20/Jersey20-Regular.ttf"),
    "bitcount": ("BitcountPropSingle.ttf",
                 "https://github.com/google/fonts/raw/main/ofl/bitcountpropsingle/BitcountPropSingle%5BCRSV%2CELSH%2CELXP%2Cslnt%2Cwght%5D.ttf"),
    "tiny5": ("Tiny5-Regular.ttf", "https://github.com/google/fonts/raw/main/ofl/tiny5/Tiny5-Regular.ttf"),
}

# A pixel font: ("pixel", family, font units per pixel, variation axes or None, ascent px, line px).
# Bitcount's axes, in order: wght, ELXP, ELSH, slnt, CRSV.
def PIXEL(family, unit, ascent, line, axes=None):
    return ("pixel", family, unit, axes, ascent, line)

# Per platform: pixel format, art sizes, and font sizes (pixels).
PLATFORMS = {
    "ds": {
        "pixel": "ds",
        "art_s": (32, 44), "art_l": (64, 87), "back_s": (14, 19),
        "logo_l": (176, 104), "logo_s": (64, 38),
        "fonts": {"tiny": ("body", 8, 500), "small": ("body", 9, 500), "body": ("body", 11, 600),
                  "label": ("label", 12, 0), "digit": ("label", 10, 0), "title": ("display", 17, 0), "big": ("display", 30, 0)},
        "icon": 14,
    },
    "psp": {
        "pixel": "psp",
        "art_s": (46, 63), "art_l": (88, 120), "back_s": (16, 22),
        "logo_l": (230, 136), "logo_s": (84, 50),
        "fonts": {"tiny": ("body", 10, 500), "small": ("body", 12, 500), "body": ("body", 14, 600),
                  "label": ("label", 16, 0), "digit": ("label", 13, 0), "title": ("display", 22, 0), "big": ("display", 40, 0)},
        "icon": 16,
    },
}
PLATFORMS["gba"] = {
    # 240 x 160 and a retro face: every glyph on the pixel grid, the cards small and crisp
    "pixel": "ds",  # the GBA reads 5:5:5 the same way (it ignores the top bit)
    "art_s": (28, 38), "art_l": (72, 99), "back_s": (8, 11),
    "logo_l": (156, 92), "logo_s": (60, 35),
    "fonts": {"tiny": PIXEL("tiny5", 128, 6, 8), "small": PIXEL("bitcount", 100, 7, 10, [400, 0, 0, 0, 0.5]),
              "body": PIXEL("bitcount", 100, 7, 10, [700, 0, 0, 0, 0.5]), "label": PIXEL("jersey10", 75, 11, 13),
              "digit": PIXEL("tiny5", 128, 6, 8), "title": PIXEL("jersey15", 50, 16, 19), "big": PIXEL("jersey20", 38, 21, 25)},
    "icon": 10, "icon_mono": True,
}
PLATFORMS["host"] = dict(PLATFORMS["ds"])  # the desktop test build draws the DS screens
PLATFORMS["host-gba"] = dict(PLATFORMS["gba"])  # ... and the GBA's

CHARSET = [c for c in range(0x20, 0x7F)] + [c for c in range(0xA0, 0x100)] + [
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212]
ICONS = [("knight", 0x265E), ("bishop", 0x265D), ("rook", 0x265C), ("queen", 0x265B), ("frozen", 0x2744)]

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def fetch(url, name):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        req = urllib.request.Request(url, headers={"User-Agent": "smashandclash-retro-assets/1.0"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    return path


def pack(pixel, r, g, b, x=0, y=0, dither=True):
    if dither:
        t = BAYER[y & 3][x & 3]
        r = min(255, r + (t >> 1))
        g = min(255, g + (t >> (2 if pixel == "psp" else 1)))
        b = min(255, b + (t >> 1))
    if pixel == "psp":  # 5:6:5, red in the low bits
        return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
    return 0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)  # DS: 5:5:5 + the opaque bit


def image_bytes(img, pixel, dither=True):
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            out += struct.pack("<H", pack(pixel, r, g, b, x, y, dither))
    return bytes(out)


def fit(img, box):
    """Scale img (with alpha) to fit inside box, keeping its shape."""
    bw, bh = box
    w, h = img.size
    s = min(bw / w, bh / h)
    return img.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)


def has_glyph(font_path, cp, cache={}):
    if font_path not in cache:
        cache[font_path] = set(TTFont(font_path).getBestCmap().keys())
    return cp in cache[font_path]


def load_font(path, size, weight):
    f = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    if weight:
        try:
            f.set_variation_by_axes([weight] if "Rubik" in path else [100, weight])
        except Exception:
            pass
    return f


def render_font(name, spec, paths):
    family, size, weight = spec
    main_path = paths[family]
    main = load_font(main_path, size, weight)
    fallback = load_font(paths["fallback"], size, weight or 600)
    math = load_font(paths["math"], size, 0)
    ascent, descent = main.getmetrics()
    glyphs, alpha = [], bytearray()
    for cp in CHARSET:
        ch = chr(cp)
        f = (main if has_glyph(main_path, cp) else fallback if has_glyph(paths["fallback"], cp)
             else math if has_glyph(paths["math"], cp) else None)
        if f is None:
            continue
        adv = round(f.getlength(ch))
        box = f.getbbox(ch, anchor="ls")  # relative to the baseline
        x0, y0, x1, y1 = box
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255, anchor="ls")
            data = im.tobytes()
        else:
            data = b""
        glyphs.append((cp, w, h, x0, y0 + ascent, adv, len(alpha)))
        alpha += data
    return {"name": name, "glyphs": glyphs, "alpha": bytes(alpha), "line": ascent + descent, "ascent": ascent}


def pixel_glyph(f, ch, K):
    """One glyph of a pixel font drawn K times too big, read back one sample per font pixel:
    (w, h, x0, y0 from the baseline, bitmap bytes, advance) with every pixel fully on or off."""
    adv = round(f.getlength(ch) / K)
    box = f.getbbox(ch, anchor="ls")
    gx0, gy0 = box[0] // K - 1, box[1] // K - 1
    gx1, gy1 = -(-box[2] // K) + 1, -(-box[3] // K) + 1
    w, h = gx1 - gx0, gy1 - gy0
    big = Image.new("L", (w * K, h * K), 0)
    ImageDraw.Draw(big).text((-gx0 * K, -gy0 * K), ch, font=f, fill=255, anchor="ls")
    px = big.load()
    on = [[px[x * K + K // 2, y * K + K // 2] >= 128 for x in range(w)] for y in range(h)]
    rows = [y for y in range(h) if any(on[y])]
    cols = [x for x in range(w) if any(on[y][x] for y in range(h))]
    if not rows:
        return 0, 0, 0, 0, b"", adv
    x0, x1, y0, y1 = cols[0], cols[-1] + 1, rows[0], rows[-1] + 1
    data = bytes(255 if on[y][x] else 0 for y in range(y0, y1) for x in range(x0, x1))
    return x1 - x0, y1 - y0, gx0 + x0, gy0 + y0, data, adv


def render_pixel_font(name, spec, paths):
    _, family, unit, axes, ascent, line = spec
    K = 8
    chain = []  # the font, then Tiny5 for what it lacks (the arrows), both on their grids
    for fam, u, ax in ((family, unit, axes), ("tiny5", 128, None)):
        path = paths[fam]
        upem = TTFont(path)["head"].unitsPerEm
        f = ImageFont.truetype(path, upem / u * K, layout_engine=ImageFont.Layout.BASIC)
        if ax:
            f.set_variation_by_axes(ax)
        chain.append((path, f))
    # the last resort: Noto, hinted and drawn in one bit at about the font's cap height
    noto = ImageFont.truetype(paths["fallback"], max(7, round(ascent * 1.2)), layout_engine=ImageFont.Layout.BASIC)
    glyphs, alpha = [], bytearray()
    for cp in CHARSET:
        ch = chr(cp)
        g = None
        for path, f in chain:
            if has_glyph(path, cp):
                g = pixel_glyph(f, ch, K)
                break
        if g is None:
            if not has_glyph(paths["fallback"], cp):
                continue
            box = noto.getbbox(ch, anchor="ls", mode="1")
            x0, y0, x1, y1 = box
            w, h = max(0, x1 - x0), max(0, y1 - y0)
            im = Image.new("1", (max(1, w), max(1, h)), 0)
            d = ImageDraw.Draw(im)
            d.fontmode = "1"
            d.text((-x0, -y0), ch, font=noto, fill=1, anchor="ls")
            data = bytes(255 if v else 0 for v in im.convert("L").tobytes()) if w and h else b""
            g = (w, h, x0, y0, data, round(noto.getlength(ch)))
        w, h, x0, y0, data, adv = g
        glyphs.append((cp, w, h, x0, y0 + ascent, adv, len(alpha)))
        alpha += data
    return {"name": name, "glyphs": glyphs, "alpha": bytes(alpha), "line": line, "ascent": ascent}


def c_bytes(data, per_line=24):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    return "\n".join(lines)


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in PLATFORMS:
        print(__doc__)
        sys.exit(1)
    plat_name, out = sys.argv[1], sys.argv[2]
    P = PLATFORMS[plat_name]
    pixel = P["pixel"]
    os.makedirs(out, exist_ok=True)

    # the deck, from the API (the same list sc.cards() returns)
    cards_path = fetch(SITE + "/api/v1/games/cards", "cards.json")
    cards = sorted(json.load(open(cards_path, encoding="utf-8"))["cards"], key=lambda c: c["id"])

    paths = {k: fetch(url, fname) for k, (fname, url) in FONTS.items()}

    art_s, art_l = bytearray(), bytearray()
    for c in cards:
        url = c["image"]
        fname = "card-" + urllib.parse.unquote(url.rsplit("/", 1)[1])
        img = Image.open(fetch(url, fname)).convert("RGB")
        art_s += image_bytes(img.resize(P["art_s"], Image.LANCZOS), pixel)
        art_l += image_bytes(img.resize(P["art_l"], Image.LANCZOS), pixel)

    back = Image.open(fetch(SITE + "/Card_Back_v2.png", "Card_Back_v2.png")).convert("RGBA")
    bg = Image.new("RGBA", back.size, (13, 26, 74, 255))  # its rounded corners over the ink
    back = Image.alpha_composite(bg, back).convert("RGB")
    back_s = image_bytes(back.resize(P["back_s"], Image.LANCZOS), pixel)

    logo = Image.open(fetch(SITE + "/logo.png", "logo.png")).convert("RGBA")
    logo = logo.crop(logo.getbbox())
    logos = {}
    for key in ("logo_l", "logo_s"):
        im = fit(logo, P[key])
        rgb = image_bytes(im, pixel, dither=False)
        a = im.getchannel("A").tobytes()
        logos[key] = (im.size, rgb, a)

    fonts = [render_pixel_font(n, spec, paths) if spec[0] == "pixel" else render_font(n, spec, paths) for n, spec in P["fonts"].items()]

    icons = []
    sym = ImageFont.truetype(paths["symbols"], P["icon"], layout_engine=ImageFont.Layout.BASIC)
    for name, cp in ICONS:
        box = sym.getbbox(chr(cp), anchor="lt")
        w, h = box[2] - box[0], box[3] - box[1]
        im = Image.new("L", (w, h), 0)
        d = ImageDraw.Draw(im)
        if P.get("icon_mono"):
            d.fontmode = "1"  # crisp, like the pixel fonts
        d.text((-box[0], -box[1]), chr(cp), font=sym, fill=255, anchor="lt")
        icons.append((name, w, h, im.tobytes()))

    # ---- binaries pulled in by assets_bin.S ----
    def write(name, data):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)

    write("art_s.bin", bytes(art_s))
    write("art_l.bin", bytes(art_l))
    # the system menu's icons
    icon = Image.open(fetch(SITE + "/Smash%26Clash_Icon.png", "Smash&Clash_Icon.png")).convert("RGBA")
    if plat_name == "ds":
        # the DS banner icon: 32 x 32, 16 colours
        im = icon.resize((32, 32), Image.LANCZOS).convert("RGB").quantize(colors=15, method=Image.MEDIANCUT)
        im.save(os.path.join(out, "icon.gif"))
    if plat_name == "psp":
        # ICON0 (144 x 80) and PIC1 (480 x 272): the XMB's tile and its backdrop
        tile = Image.new("RGBA", (144, 80), (1, 109, 203, 255))
        logo80 = fit(logo, (128, 72))
        tile.alpha_composite(logo80, ((144 - logo80.width) // 2, (80 - logo80.height) // 2))
        tile.save(os.path.join(out, "ICON0.PNG"))
        pic = Image.new("RGB", (480, 272))
        for y in range(272):
            t = y / 271
            pic.paste((round(10 + (1 - 10) * t), round(190 + (109 - 190) * t), round(255 + (203 - 255) * t)), (0, y, 480, y + 1))
        pic = pic.convert("RGBA")
        big = fit(logo, (300, 180))
        pic.alpha_composite(big, ((480 - big.width) // 2, (272 - big.height) // 2))
        pic.convert("RGB").save(os.path.join(out, "PIC1.PNG"))

    rel = out.replace("\\", "/").rstrip("/")  # .incbin paths are relative to where make runs
    with open(os.path.join(out, "assets_bin.s"), "w") as f:
        f.write("/* Card art, pulled in whole (generated by tools/make_assets.py). */\n")
        for sym_name, fname in (("snc_art_s", "art_s.bin"), ("snc_art_l", "art_l.bin")):  # one section each: unused art drops out
            f.write(f"    .section .rodata.{sym_name},\"a\"\n    .global {sym_name}\n    .balign 4\n{sym_name}:\n    .incbin \"{rel}/{fname}\"\n")

    # ---- the header ----
    n = len(cards)
    h = []
    h.append("// Generated by tools/make_assets.py: do not edit.")
    h.append("#ifndef SNC_ASSETS_H\n#define SNC_ASSETS_H\n#include <stdint.h>\n#include \"snc_gfx.h\"\n")
    h.append(f"#define SNC_CARDS {n}")
    h.append(f"#define ART_S_W {P['art_s'][0]}\n#define ART_S_H {P['art_s'][1]}")
    h.append(f"#define ART_L_W {P['art_l'][0]}\n#define ART_L_H {P['art_l'][1]}")
    h.append(f"#define BACK_S_W {P['back_s'][0]}\n#define BACK_S_H {P['back_s'][1]}")
    for key in ("logo_l", "logo_s"):
        (w, hh), _, _ = logos[key]
        h.append(f"#define {key.upper()}_W {w}\n#define {key.upper()}_H {hh}")
    h.append("")
    h.append("typedef struct { const char *name; uint8_t effect; int8_t top, right, bottom, left; const char *color; } snc_card_info;")
    h.append("extern const snc_card_info snc_cards[SNC_CARDS];")
    h.append("extern const uint16_t snc_art_s[];  // SNC_CARDS images, ART_S_W x ART_S_H each")
    h.append("extern const uint16_t snc_art_l[];")
    h.append("extern const uint16_t snc_back_s[];")
    for key in ("logo_l", "logo_s"):
        h.append(f"extern const uint16_t snc_{key}[];\nextern const uint8_t snc_{key}_a[];")
    for f in fonts:
        h.append(f"extern const snc_font font_{f['name']};")
    for name, w, hh, _ in icons:
        h.append(f"#define ICON_{name.upper()}_W {w}\n#define ICON_{name.upper()}_H {hh}\nextern const uint8_t icon_{name}[];")
    h.append("\n// The card's index in snc_cards / the art, or -1.\nint snc_card_index(const char *name);")
    h.append("// The same, telling apart cards that share a name (there are two Lizzies) by their printed sides.")
    h.append("int snc_card_find(const char *name, int top, int right, int bottom, int left);\n#endif")
    with open(os.path.join(out, "assets.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(h) + "\n")

    # ---- the small data ----
    c = ["// Generated by tools/make_assets.py: do not edit.", '#include <string.h>', '#include "assets.h"', ""]
    c.append("const snc_card_info snc_cards[SNC_CARDS] = {")
    for card in cards:
        eff = 1 if card["kind"] == "effect" else 0
        c.append('    {%s, %d, %d, %d, %d, %d, "%s"},' % (json.dumps(card["name"]), eff, card.get("top", 0), card.get("right", 0),
                                                     card.get("bottom", 0), card.get("left", 0), card.get("color", "")))
    c.append("};\n")
    c.append("int snc_card_index(const char *name) {\n    for (int i = 0; i < SNC_CARDS; i++)\n        if (!strcmp(snc_cards[i].name, name)) return i;\n    return -1;\n}\n")
    c.append("int snc_card_find(const char *name, int top, int right, int bottom, int left) {\n    int first = -1;\n"
             "    for (int i = 0; i < SNC_CARDS; i++) {\n        if (strcmp(snc_cards[i].name, name)) continue;\n        if (first < 0) first = i;\n"
             "        if (snc_cards[i].top == top && snc_cards[i].right == right && snc_cards[i].bottom == bottom && snc_cards[i].left == left) return i;\n"
             "    }\n    return first;\n}\n")
    c.append("const uint16_t snc_back_s[] = {" + ",".join(str(v) for v in struct.unpack(f"<{len(back_s)//2}H", back_s)) + "};")
    for key in ("logo_l", "logo_s"):
        _, rgb, a = logos[key]
        c.append(f"const uint16_t snc_{key}[] = {{" + ",".join(str(v) for v in struct.unpack(f"<{len(rgb)//2}H", rgb)) + "};")
        c.append(f"const uint8_t snc_{key}_a[] = {{\n{c_bytes(a)}\n}};")
    for f in fonts:
        nm = f["name"]
        c.append(f"static const uint8_t font_{nm}_alpha[] = {{\n{c_bytes(f['alpha'])}\n}};")
        c.append(f"static const snc_glyph font_{nm}_glyphs[] = {{")
        for (cp, w, hh, xo, yo, adv, off) in f["glyphs"]:
            c.append(f"    {{{cp}, {w}, {hh}, {xo}, {yo}, {adv}, {off}}},")
        c.append("};")
        c.append(f"const snc_font font_{nm} = {{font_{nm}_glyphs, {len(f['glyphs'])}, font_{nm}_alpha, {f['line']}, {f['ascent']}}};\n")
    for name, w, hh, data in icons:
        c.append(f"const uint8_t icon_{name}[] = {{\n{c_bytes(data)}\n}};")
    with open(os.path.join(out, "assets_data.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(c) + "\n")
    print(f"{plat_name}: {n} cards, art {len(art_s) // 1024}+{len(art_l) // 1024} KB, {len(fonts)} fonts -> {out}")


if __name__ == "__main__":
    main()
