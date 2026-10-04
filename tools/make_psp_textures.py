#!/usr/bin/env python3
"""Builds the PSP client's textures: everything the GPU draws, from the real game's art.

  python3 tools/make_psp_textures.py OUT_DIR

Every texture is 8-bit indexed (GU_PSM_T8) with its own 256-colour RGBA palette, which
keeps the whole set (51 cards, 46 champions, the board, the UI) around 4 MB. Writes
OUT_DIR/textures.bin (pulled into the EBOOT with .incbin) and OUT_DIR/textures.h (the
table: where each texture starts, its size, and the glyphs of each font atlas).

Needs Python 3, Pillow, fontTools; the downloads are shared with make_assets.py
(tools/cache/, run that first or let this fetch them).
"""
import json
import math
import os
import random
import struct
import sys
import urllib.parse

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_assets as MA  # noqa: E402  (shared fetch() and font paths)

SITE = MA.SITE
CARD_W, CARD_H = 94, 128  # the card inside its 128 x 128 texture
CHAMP = 256              # VS renders: 256 x 256
AVATAR = 64


# ------------------------------------------------------------------ helpers

def round_mask(w, h, r, scale=4):
    big = Image.new("L", (w * scale, h * scale), 0)
    ImageDraw.Draw(big).rounded_rectangle([0, 0, w * scale - 1, h * scale - 1], r * scale, fill=255)
    return big.resize((w, h), Image.LANCZOS)


def t8(img, colors=256):
    """RGBA image -> (indices bytes, palette ABGR8888 words) for GU_PSM_T8 + GU_PSM_8888 CLUT."""
    img = img.convert("RGBA")
    # premultiply-ish: fully transparent pixels share one colour, so the palette is not wasted on them
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            if px[x, y][3] == 0:
                px[x, y] = (0, 0, 0, 0)
    q = img.quantize(colors=colors, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.FLOYDSTEINBERG)
    pal = q.getpalette("RGBA") or []
    pal += [0] * (1024 - len(pal))
    words = [pal[i] | (pal[i + 1] << 8) | (pal[i + 2] << 16) | (pal[i + 3] << 24) for i in range(0, 1024, 4)]
    return q.tobytes(), words


def alpha_t8(mask):
    """An alpha-only image (L) -> indices = alpha, palette = white with that alpha."""
    words = [0x00FFFFFF | (a << 24) for a in range(256)]
    return mask.convert("L").tobytes(), words


class Pack:
    def __init__(self):
        self.data = bytearray()
        self.entries = []  # (name, w, h, data_off, pal_off)

    def add(self, name, w, h, idx, pal):
        assert len(idx) == w * h, (name, len(idx), w, h)
        while len(self.data) % 64:
            self.data.append(0)
        pal_off = len(self.data)
        self.data += struct.pack("<256I", *pal)
        data_off = len(self.data)
        self.data += idx
        self.entries.append((name, w, h, data_off, pal_off))
        return len(self.entries) - 1


def fit(img, box):
    bw, bh = box
    s = min(bw / img.width, bh / img.height)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)


# ------------------------------------------------------------------ the board

WOOD = (233, 203, 150)


def wood_texture(w, h, seed=7):
    rnd = random.Random(seed)
    img = Image.new("RGB", (w, h), WOOD)
    px = img.load()
    phase = [rnd.random() * 6.28 for _ in range(6)]
    for y in range(h):
        for x in range(w):
            g = (math.sin(y * 0.11 + math.sin(x * 0.013 + phase[0]) * 2.2 + phase[1]) * 0.5 + 0.5)
            g = g ** 3
            n = (math.sin(x * 0.9 + y * 0.31 + phase[2]) + math.sin(x * 0.27 - y * 0.7 + phase[3])) * 0.02
            k = 1.0 - g * 0.07 + n
            r, gg, b = WOOD
            px[x, y] = (min(255, int(r * k)), min(255, int(gg * k)), min(255, int(b * k)))
    return img


def engraved_logo(logo, box):
    """The Smash&Clash logo carved into the wood: the letters' black outlines burned in as
    grooves, the letter faces cut down into the wood, a shadow on the top-left wall of the
    cut and light on the bottom-right one. Drawn at 4x, then scaled down."""
    S = 4
    lg = fit(logo, (box[0] * S, box[1] * S))
    w, h = lg.size
    alpha = lg.getchannel("A")
    lum = lg.convert("L")
    # the outline strokes that draw the letters
    ap, lp = alpha.load(), lum.load()
    outline = Image.new("L", (w, h), 0)
    op = outline.load()
    for y in range(h):
        for x in range(w):
            if ap[x, y] > 110 and lp[x, y] < 100:
                op[x, y] = 255
    # everything the outline does not enclose is outside; what it encloses are the letter faces
    region = Image.new("L", (w + 2, h + 2), 0)
    region.paste(outline, (1, 1))
    ImageDraw.floodfill(region, (0, 0), 128, thresh=0)
    region = region.crop((1, 1, w + 1, h + 1))
    faces = region.point(lambda v: 255 if v == 0 else 0)

    def shifted(mask, dx, dy):
        out = Image.new("L", mask.size, 0)
        out.paste(mask, (dx, dy))
        return out

    d = 2 * S  # how deep the cut looks
    shadow = ImageChops.subtract(faces, shifted(faces, d, d))   # the top-left wall of the cut
    light = ImageChops.subtract(faces, shifted(faces, -d, -d))  # the bottom-right wall
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))

    def paint(mask, rgb, a):
        tint = Image.new("RGBA", (w, h), rgb + (0,))
        tint.putalpha(mask.point(lambda v: v * a // 255))
        layer.alpha_composite(tint)

    paint(faces, (196, 152, 96), 175)       # the cut-down floor of each letter
    paint(shadow, (128, 86, 44), 190)       # its shaded wall
    paint(light, (255, 238, 204), 200)      # its lit wall
    paint(outline, (112, 74, 36), 225)      # the burned outline
    return layer.resize((max(1, w // S), max(1, h // S)), Image.LANCZOS)


def board_texture(logo):
    """The folding wooden board seen from above: 5 x 3 slots, the crease, coordinates for both seats."""
    W, H = 512, 420
    img = wood_texture(W, H).convert("RGBA")
    d = ImageDraw.Draw(img)
    border = 30
    tile_w, tile_h, gap = (W - 2 * border - 4 * 8) / 5, (H - 2 * border - 2 * 8) / 3, 8
    # the raised rim
    d.rounded_rectangle([1, 1, W - 2, H - 2], 18, outline=(178, 135, 82), width=3)
    d.rounded_rectangle([5, 5, W - 6, H - 6], 15, outline=(246, 226, 188), width=2)
    font = ImageFont.truetype(MA.fetch(*reversed(MA.FONTS["label"])), 22)
    cols = "ABCDE"
    carved = engraved_logo(logo, (int(tile_w * 0.9), int(tile_h * 0.62)))
    for r in range(3):
        for c in range(5):
            x0 = border + c * (tile_w + gap)
            y0 = border + r * (tile_h + gap)
            box = [x0, y0, x0 + tile_w, y0 + tile_h]
            # the recessed slot: darker floor, shadow on the top-left lip, light on the bottom-right
            d.rounded_rectangle([box[0] - 1, box[1] - 1, box[2] + 1, box[3] + 1], 9, fill=(196, 156, 104))
            d.rounded_rectangle([box[0] + 1, box[1] + 1, box[2] + 1, box[3] + 1], 8, fill=(250, 232, 196))
            d.rounded_rectangle([box[0] + 1, box[1] + 1, box[2] - 1, box[3] - 1], 8, fill=(226, 194, 140))
            mark = carved if (r + c) % 2 == 0 else carved.rotate(180)  # every other slot reads for the far seat
            img.alpha_composite(mark, (int(x0 + (tile_w - mark.width) / 2), int(y0 + (tile_h - mark.height) / 2)))
    # the crease down the middle, with its two hinge marks
    cx = W // 2
    d.line([(cx, 6), (cx, H - 7)], fill=(150, 110, 62), width=2)
    d.line([(cx + 2, 6), (cx + 2, H - 7)], fill=(250, 232, 196), width=1)
    for y in (border // 2, H - border // 2):
        d.line([(cx - 9, y), (cx + 9, y)], fill=(150, 110, 62), width=2)
    # coordinates: upright for seat A on the near (bottom) edge and the left; turned for seat B
    ink = (138, 98, 52)
    for c in range(5):
        x = border + c * (tile_w + gap) + tile_w / 2
        d.text((x, H - border / 2), cols[c], font=font, fill=ink, anchor="mm")
        t = Image.new("RGBA", (40, 40), (0, 0, 0, 0))
        ImageDraw.Draw(t).text((20, 20), cols[c], font=font, fill=ink, anchor="mm")
        t = t.rotate(180)
        img.alpha_composite(t, (int(x - 20), int(border / 2 - 20)))
    for r in range(3):
        y = border + (2 - r) * (tile_h + gap) + tile_h / 2
        d.text((border / 2, y), str(r + 1), font=font, fill=ink, anchor="mm")
        t = Image.new("RGBA", (40, 40), (0, 0, 0, 0))
        ImageDraw.Draw(t).text((20, 20), str(r + 1), font=font, fill=ink, anchor="mm")
        t = t.rotate(180)
        img.alpha_composite(t, (int(W - border / 2 - 20), int(y - 20)))
    full = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    full.paste(img, (0, 0))
    return full, (W, H, border, tile_w, tile_h, gap)


def chess_tile(piece_cp, symbols_path):
    """A purple glossy tile with a white disc and the piece, like the web edition's."""
    S = 64
    big = 4
    img = Image.new("RGBA", (S * big, S * big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i in range(S * big):  # a vertical gloss gradient
        t = i / (S * big)
        col = (int(176 - 50 * t), int(136 - 60 * t), int(255 - 30 * t), 255)
        d.line([(0, i), (S * big, i)], fill=col)
    mask = round_mask(S * big, S * big, 10 * big, 1)
    img.putalpha(mask)
    d.ellipse([S * big * 0.2, S * big * 0.2, S * big * 0.8, S * big * 0.8], fill=(255, 255, 255, 240))
    f = ImageFont.truetype(symbols_path, int(S * big * 0.42))
    d.text((S * big / 2, S * big / 2), chr(piece_cp), font=f, fill=(91, 37, 214, 255), anchor="mm")
    return img.resize((S, S), Image.LANCZOS)


# ------------------------------------------------------------------ fonts

FONT_SPECS = [  # name, family, size, weight, full character set?, atlas
    ("small", "body", 12, 500, True, 0), ("body", "body", 14, 600, True, 0), ("label", "label", 16, 0, True, 0),
    ("label_l", "label", 21, 0, False, 0), ("title", "display", 28, 0, False, 1), ("huge", "display", 46, 0, False, 1),
]
SHORT_SET = list(range(0x20, 0x7F)) + [0xB7, 0x2013, 0x2026, 0x2192]  # the display sizes: ASCII and a few marks


def font_atlas(paths, which):
    W, H = 512, 512
    atlas = Image.new("L", (W, H), 0)
    x = y = 0
    row_h = 0
    fonts = []
    for name, fam, size, weight, full, at in FONT_SPECS:
        if at != which:
            continue
        main = MA.load_font(paths[fam], size, weight)
        fb = MA.load_font(paths["fallback"], size, weight or 600)
        math_f = MA.load_font(paths["math"], size, 0)
        ascent, descent = main.getmetrics()
        glyphs = []
        for cp in (MA.CHARSET if full else SHORT_SET):
            ch = chr(cp)
            f = (main if MA.has_glyph(paths[fam], cp) else fb if MA.has_glyph(paths["fallback"], cp)
                 else math_f if MA.has_glyph(paths["math"], cp) else None)
            if f is None:
                continue
            adv = f.getlength(ch)
            x0, y0, x1, y1 = f.getbbox(ch, anchor="ls")
            w, h = max(0, x1 - x0), max(0, y1 - y0)
            if w and h:
                if x + w + 1 > W:
                    x, y, row_h = 0, y + row_h + 1, 0
                g = Image.new("L", (w, h), 0)
                ImageDraw.Draw(g).text((-x0, -y0), ch, font=f, fill=255, anchor="ls")
                atlas.paste(g, (x, y))
                glyphs.append((cp, x, y, w, h, x0, y0 + ascent, adv))
                x += w + 1
                row_h = max(row_h, h)
            else:
                glyphs.append((cp, 0, 0, 0, 0, 0, 0, adv))
        fonts.append((name, glyphs, ascent + descent, ascent))
    assert y + row_h <= H, "font atlas overflow"
    return atlas, fonts


# ------------------------------------------------------------------ main

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/tex"
    os.makedirs(out, exist_ok=True)
    pack = Pack()
    paths = {k: MA.fetch(url, fname) for k, (fname, url) in MA.FONTS.items()}
    cards = sorted(json.load(open(MA.fetch(SITE + "/api/v1/games/cards", "cards.json"), encoding="utf-8"))["cards"], key=lambda c: c["id"])
    logo = Image.open(MA.fetch(SITE + "/logo.png", "logo.png")).convert("RGBA")
    logo = logo.crop(logo.getbbox())

    # cards: rounded, in 128 x 128 (the card is CARD_W x CARD_H in the middle)
    card_mask = round_mask(CARD_W, CARD_H, 7)
    names = []
    for c in cards:
        url = c["image"]
        art = Image.open(MA.fetch(url, "card-" + urllib.parse.unquote(url.rsplit("/", 1)[1]))).convert("RGBA")
        art = art.resize((CARD_W, CARD_H), Image.LANCZOS)
        art.putalpha(ImageChops.multiply(art.getchannel("A"), card_mask))
        tex = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
        tex.paste(art, ((128 - CARD_W) // 2, 0))
        pack.add("card_" + c["name"], 128, 128, *t8(tex))
        names.append(c["name"])
    back = Image.open(MA.fetch(SITE + "/Card_Back_v2.png", "Card_Back_v2.png")).convert("RGBA").resize((CARD_W, CARD_H), Image.LANCZOS)
    back.putalpha(ImageChops.multiply(back.getchannel("A"), card_mask))
    tex = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
    tex.paste(back, ((128 - CARD_W) // 2, 0))
    pack.add("back", 128, 128, *t8(tex))

    # champions: the VS renders (facing right and left) and round avatars
    champs = [c for c in cards if c["kind"] == "character"]
    for c in champs:
        cid = c["id"]
        for side, path in (("r", f"/vs/{cid}.webp"),):  # the left-facing pose is this one mirrored on the GPU
            img = Image.open(MA.fetch(SITE + path, f"vs-{side}-{cid}.webp")).convert("RGBA")
            img = img.crop(img.getbbox())
            img = fit(img, (CHAMP, CHAMP))
            tex = Image.new("RGBA", (CHAMP, CHAMP), (0, 0, 0, 0))
            tex.paste(img, ((CHAMP - img.width) // 2, CHAMP - img.height), img)
            pack.add(f"vs{side}_{cid}", CHAMP, CHAMP, *t8(tex))
        stem = urllib.parse.quote(c["name"].replace(" ", "_").replace(".", ""))
        try:
            body = Image.open(MA.fetch(f"{SITE}/Characters-webp/{cid:02d}_{stem}.webp", f"char-{cid}.webp")).convert("RGBA")
        except Exception:
            body = img
        body = body.crop(body.getbbox())
        side_len = int(body.width * 0.62)
        head = body.crop(((body.width - side_len) // 2, 0, (body.width + side_len) // 2, side_len)).resize((AVATAR, AVATAR), Image.LANCZOS)
        av = Image.new("RGBA", (AVATAR, AVATAR), (95, 166, 255, 255))
        av.alpha_composite(head)
        av.putalpha(round_mask(AVATAR, AVATAR, AVATAR // 2))
        pack.add(f"avatar_{cid}", AVATAR, AVATAR, *t8(av))

    board, geo = board_texture(logo)
    pack.add("board", 512, 512, *t8(board))
    for name, cp in (("knight", 0x265E), ("bishop", 0x265D), ("rook", 0x265C), ("queen", 0x265B)):
        pack.add("chess_" + name, 64, 64, *t8(chess_tile(cp, paths["symbols"])))

    # the logo, the UI shapes and the PlayStation glyphs
    lg = fit(logo, (256, 128))
    tex = Image.new("RGBA", (256, 128), (0, 0, 0, 0))
    tex.paste(lg, ((256 - lg.width) // 2, (128 - lg.height) // 2), lg)
    pack.add("logo", 256, 128, *t8(tex))
    pack.add("circle", 64, 64, *alpha_t8(round_mask(64, 64, 32)))
    glow = Image.new("L", (64, 64), 0)
    gp = glow.load()
    for yy in range(64):
        for xx in range(64):
            r = math.hypot(xx - 31.5, yy - 31.5) / 32
            gp[xx, yy] = int(255 * max(0.0, 1 - r) ** 2)
    pack.add("glow", 64, 64, *alpha_t8(glow))
    rays = Image.new("L", (128, 128), 0)
    rd = ImageDraw.Draw(rays)
    for k in range(16):
        a0 = k * math.pi / 8
        rd.polygon([(64, 64), (64 + 200 * math.cos(a0), 64 + 200 * math.sin(a0)),
                    (64 + 200 * math.cos(a0 + math.pi / 16), 64 + 200 * math.sin(a0 + math.pi / 16))], fill=255)
    pack.add("rays", 128, 128, *alpha_t8(rays))
    # a rounded tile outline and a soft tile glow (stretched over a board tile in 3D)
    big = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(big).rounded_rectangle([10, 10, 245, 245], 40, outline=255, width=26)
    pack.add("tileframe", 64, 64, *alpha_t8(big.resize((64, 64), Image.LANCZOS)))
    soft = Image.new("L", (64, 64), 0)
    ImageDraw.Draw(soft).rounded_rectangle([10, 10, 53, 53], 10, fill=255)
    pack.add("tileglow", 64, 64, *alpha_t8(soft.filter(ImageFilter.GaussianBlur(5))))
    snow = Image.new("L", (64, 64), 0)
    ImageDraw.Draw(snow).text((32, 32), "❄", font=ImageFont.truetype(paths["symbols"], 48), fill=255, anchor="mm")
    pack.add("frozen", 64, 64, *alpha_t8(snow))
    for gname, col, shape in (("cross", (143, 184, 255), "x"), ("circleg", (255, 122, 122), "o"), ("triangle", (92, 224, 200), "t"),
                              ("square", (255, 138, 240), "s")):
        g = Image.new("RGBA", (128, 128), (0, 0, 0, 0))
        gd = ImageDraw.Draw(g)
        gd.ellipse([2, 2, 125, 125], fill=(6, 16, 47, 240))
        w = 12
        if shape == "x":
            gd.line([(38, 38), (90, 90)], fill=col, width=w)
            gd.line([(90, 38), (38, 90)], fill=col, width=w)
        elif shape == "o":
            gd.ellipse([34, 34, 94, 94], outline=col, width=w)
        elif shape == "t":
            gd.polygon([(64, 30), (98, 90), (30, 90)], outline=col, width=w)
        else:
            gd.rectangle([36, 36, 92, 92], outline=col, width=w)
        pack.add("glyph_" + gname, 32, 32, *t8(g.resize((32, 32), Image.LANCZOS)))

    fonts = []
    for which, tex_name in ((0, "font"), (1, "font_big")):
        atlas, fs_ = font_atlas(paths, which)
        tex_index = pack.add(tex_name, 512, 512, *alpha_t8(atlas))
        fonts += [(n, g, l, a, tex_index) for (n, g, l, a) in fs_]

    with open(os.path.join(out, "textures.bin"), "wb") as f:
        f.write(pack.data)
    rel = out.replace("\\", "/").rstrip("/")
    with open(os.path.join(out, "textures_bin.s"), "w") as f:
        f.write("/* The PSP textures (generated by tools/make_psp_textures.py). */\n    .section .rodata\n")
        f.write(f"    .global snc_tex_data\n    .balign 64\nsnc_tex_data:\n    .incbin \"{rel}/textures.bin\"\n")

    h = ["// Generated by tools/make_psp_textures.py: do not edit.", "#ifndef SNC_TEXTURES_H", "#define SNC_TEXTURES_H", "#include <stdint.h>", ""]
    h.append("typedef struct { const char *name; uint16_t w, h; uint32_t data, pal; } snc_tex_t;")
    h.append("typedef struct { uint16_t cp; uint16_t x, y; uint8_t w, h; int8_t xo, yo; float adv; } snc_tglyph;")
    h.append("typedef struct { const snc_tglyph *g; int n; int line, ascent; int tex; } snc_tfont;")
    h.append("extern const uint8_t snc_tex_data[];")
    h.append(f"#define SNC_TEX_COUNT {len(pack.entries)}")
    h.append("extern const snc_tex_t snc_tex[SNC_TEX_COUNT];")
    for i, (name, *_rest) in enumerate(pack.entries):
        ident = "TEX_" + "".join(ch if ch.isalnum() else "_" for ch in name.upper())
        if name.startswith("card_"):
            continue
        h.append(f"#define {ident} {i}")
    h.append(f"#define TEX_CARD0 0  // cards in deck order (snc_cards in assets.h)")
    h.append(f"#define TEX_CHAMP0 {[e[0] for e in pack.entries].index('vsr_1')}  // per character: vsr, avatar")
    h.append(f"#define CARD_TW {CARD_W}\n#define CARD_TH {CARD_H}")
    W, H, border, tw, th, gap = geo
    h.append(f"#define BOARD_TEX_W {W}\n#define BOARD_TEX_H {H}\n#define BOARD_BORDER {border}\n#define BOARD_TILE_W {tw:.3f}f\n#define BOARD_TILE_H {th:.3f}f\n#define BOARD_GAP {gap}")
    for name, *_ in fonts:
        h.append(f"extern const snc_tfont tfont_{name};")
    h.append("#endif")
    with open(os.path.join(out, "textures.h"), "w") as f:
        f.write("\n".join(h) + "\n")

    c = ["// Generated by tools/make_psp_textures.py: do not edit.", '#include "textures.h"', ""]
    c.append("const snc_tex_t snc_tex[SNC_TEX_COUNT] = {")
    for name, w, hh, doff, poff in pack.entries:
        c.append(f'    {{{json.dumps(name)}, {w}, {hh}, {doff}, {poff}}},')
    c.append("};")
    for name, glyphs, line, ascent, tex_index in fonts:
        c.append(f"static const snc_tglyph tg_{name}[] = {{")
        for (cp, x, y, w, hh, xo, yo, adv) in glyphs:
            c.append(f"    {{{cp}, {x}, {y}, {w}, {hh}, {xo}, {yo}, {adv:.2f}f}},")
        c.append("};")
        c.append(f"const snc_tfont tfont_{name} = {{tg_{name}, {len(glyphs)}, {line}, {ascent}, {tex_index}}};")
    with open(os.path.join(out, "textures_data.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(c) + "\n")
    print(f"psp textures: {len(pack.entries)} textures, {len(pack.data) // 1024} KB -> {out}")


if __name__ == "__main__":
    main()
