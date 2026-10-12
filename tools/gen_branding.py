"""Builds the app icon, the splash screens and the logo used in the UI from assets/icon_source.png.
Outputs: icon.png (128x128, also embedded in the binary), tv.png (1280x720), drc.png (854x480), assets/icon.png (512x512).
Usage: python tools/gen_branding.py  (needs pillow)"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageChops, ImageFilter

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
logo = Image.open(os.path.join(ROOT, 'assets', 'icon_source.png')).convert('RGB')
FONT = os.path.join(ROOT, 'content', 'font.ttf')


def tile(size, radius=0.0):
    t = logo.resize((size, size), Image.LANCZOS).convert('RGBA')
    if radius > 0:
        m = Image.new('L', (size * 4, size * 4), 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, size * 4 - 1, size * 4 - 1], int(size * 4 * radius), fill=255)
        t.putalpha(m.resize((size, size), Image.LANCZOS))
    return t


def splash(W, H):
    img = Image.new('RGB', (W, H))
    px = img.load()
    for y in range(H):  # same gradient as the UI background
        t = y / (H - 1)
        c = (int(18 - 11 * t), int(14 - 8 * t), int(23 - 13 * t))
        for x in range(W):
            px[x, y] = c
    glow = Image.new('RGB', (W, H), (0, 0, 0))
    gs = int(H * 0.62)
    ImageDraw.Draw(glow).ellipse([W // 2 - gs, int(H * 0.40) - gs, W // 2 + gs, int(H * 0.40) + gs], fill=(70, 22, 30))
    img = ImageChops.add(img, glow.filter(ImageFilter.GaussianBlur(H * 0.12)))
    ts = int(H * 0.50)
    t = tile(ts, radius=0.18)
    img.paste(t, ((W - ts) // 2, int(H * 0.13)), t)
    d = ImageDraw.Draw(img)
    big = ImageFont.truetype(FONT, int(H * 0.115))
    small = ImageFont.truetype(FONT, int(H * 0.042))
    ty = int(H * 0.13) + ts + int(H * 0.035)
    tw = d.textlength('MiiTune', font=big)
    d.text(((W - tw) / 2, ty), 'MiiTune', font=big, fill=(246, 244, 248))
    sub = 'YouTube Music for Wii U'
    sw = d.textlength(sub, font=small)
    d.text(((W - sw) / 2, ty + int(H * 0.145)), sub, font=small, fill=(165, 160, 180))
    return img


tile(128).save(os.path.join(ROOT, 'icon.png'))
logo.resize((512, 512), Image.LANCZOS).save(os.path.join(ROOT, 'assets', 'icon.png'), optimize=True)
splash(1280, 720).save(os.path.join(ROOT, 'tv.png'))
splash(854, 480).save(os.path.join(ROOT, 'drc.png'))
print('done')
