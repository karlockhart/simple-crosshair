"""Generates neonsight.ico (16-256 px), a 256 px PNG preview, and the MSIX/Store logos
in store/Assets.

Large sizes: dark rounded badge, neon crosshair with a segmented ring and a soft glow,
rendered at 4x and downsampled. Sizes <= 24 px are drawn pixel by pixel so the tray and
taskbar icons stay crisp. Requires Pillow:  python tools/make_icon.py
"""
import io
import os
import struct

from PIL import Image, ImageDraw, ImageFilter

GREEN = (34, 255, 102)
BG_TOP = (27, 36, 52)
BG_BOTTOM = (10, 12, 17)
BORDER = (46, 54, 70)
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_ICO = os.path.join(HERE, "..", "neonsight.ico")
OUT_PNG = os.path.join(HERE, "..", "icon-preview.png")
OUT_ASSETS = os.path.join(HERE, "..", "store", "Assets")


def badge(size, inset, radius, border):
    """Rounded square with a vertical gradient and a thin border."""
    grad = Image.new("RGBA", (size, size))
    px = grad.load()
    for y in range(size):
        t = y / max(1, size - 1)
        c = tuple(round(BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t) for i in range(3))
        for x in range(size):
            px[x, y] = c + (255,)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (inset, inset, size - 1 - inset, size - 1 - inset), radius, fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    if border:
        ImageDraw.Draw(out).rounded_rectangle(
            (inset, inset, size - 1 - inset, size - 1 - inset), radius, outline=BORDER + (255,), width=border)
    return out


def large(size):
    ss = size * 4
    s = float(ss)
    c = s / 2
    img = badge(ss, round(s * 0.04), round(s * 0.22), max(4, round(s * 0.012)))

    mark = Image.new("RGBA", (ss, ss), (0, 0, 0, 0))
    d = ImageDraw.Draw(mark)
    col = GREEN + (255,)
    # Segmented ring, broken where the arms pass through.
    r, w = s * 0.30, round(s * 0.045)
    box = (c - r, c - r, c + r, c + r)
    for start in (0, 90, 180, 270):
        d.arc(box, start + 16, start + 74, fill=col, width=w)
    # Four arms with a centre gap, plus a dot.
    t = s * 0.035
    for a0, a1 in ((0.11, 0.41),):
        d.rectangle((c - s * a1, c - t, c - s * a0, c + t), fill=col)
        d.rectangle((c + s * a0, c - t, c + s * a1, c + t), fill=col)
        d.rectangle((c - t, c - s * a1, c + t, c - s * a0), fill=col)
        d.rectangle((c - t, c + s * a0, c + t, c + s * a1), fill=col)
    dr = s * 0.04
    d.ellipse((c - dr, c - dr, c + dr, c + dr), fill=col)

    glow = mark.filter(ImageFilter.GaussianBlur(s * 0.035))
    glow.putalpha(glow.getchannel("A").point(lambda v: int(v * 0.9)))
    img.alpha_composite(glow)
    img.alpha_composite(mark)
    return img.resize((size, size), Image.LANCZOS)


def small(size):
    """Pixel-exact simplified mark: cross with a gap and a 2x2 centre dot."""
    img = badge(size, 0, max(2, size // 5), 0)
    d = ImageDraw.Draw(img)
    col = GREEN + (255,)
    c = size // 2                      # arms sit on rows/cols c-1 and c
    gap = 2 if size < 24 else 3
    edge = 2 if size < 20 else 3
    d.rectangle((edge, c - 1, c - 1 - gap, c), fill=col)
    d.rectangle((c + gap, c - 1, size - 1 - edge, c), fill=col)
    d.rectangle((c - 1, edge, c, c - 1 - gap), fill=col)
    d.rectangle((c - 1, c + gap, c, size - 1 - edge), fill=col)
    d.rectangle((c - 1, c - 1, c, c), fill=col)
    return img


def write_ico(path, images):
    """ICO with PNG-compressed entries (supported since Windows Vista)."""
    blobs = []
    for im in images:
        buf = io.BytesIO()
        im.save(buf, "PNG")
        blobs.append(buf.getvalue())
    offset = 6 + 16 * len(images)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for im, blob in zip(images, blobs):
            w = im.width if im.width < 256 else 0
            f.write(struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(blob), offset))
            offset += len(blob)
        for blob in blobs:
            f.write(blob)


def icon(size):
    return small(size) if size <= 24 else large(size)


def padded(size, fill):
    """The icon at `fill` of the canvas, centred on transparency (for tiles)."""
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    inner = icon(round(size * fill))
    off = (size - inner.width) // 2
    out.alpha_composite(inner, (off, off))
    return out


def write_store_assets():
    """MSIX logos for store/Assets (see pack.ps1). The icon has its own dark badge, so the
    taskbar/Start versions are 'unplated' (no accent-colour plate behind them)."""
    os.makedirs(OUT_ASSETS, exist_ok=True)
    files = {}
    for scale in (100, 125, 150, 200, 400):
        files[f"Square44x44Logo.scale-{scale}.png"] = icon(44 * scale // 100)
        files[f"Square150x150Logo.scale-{scale}.png"] = padded(150 * scale // 100, 0.62)
        files[f"StoreLogo.scale-{scale}.png"] = icon(50 * scale // 100)
    for n in (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256):
        for suffix in ("", "_altform-unplated", "_altform-lightunplated"):
            files[f"Square44x44Logo.targetsize-{n}{suffix}.png"] = icon(n)
    for name, im in files.items():
        im.save(os.path.join(OUT_ASSETS, name))
    return len(files)


if __name__ == "__main__":
    images = [icon(n) for n in SIZES]
    write_ico(OUT_ICO, images)
    images[-1].save(OUT_PNG)
    count = write_store_assets()
    print("wrote", os.path.normpath(OUT_ICO), os.path.normpath(OUT_PNG),
          f"and {count} logos in", os.path.normpath(OUT_ASSETS))
