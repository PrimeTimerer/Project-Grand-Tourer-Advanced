# Draws the port's icon (a rev counter with its needle at the red line: this project's own drawing, no game artwork)
# and writes it as a Windows .ico with every usual size: 256 as PNG, the smaller ones as 32-bit bitmaps.
#   python -X utf8 make_icon.py <out.ico> [<preview.png>]
# The same file is the launcher's icon (build_launcher.bat) and the game exe's (app.rc -> app.ico).
import io, math, struct, sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024                      # drawn large, then scaled down
SIZES = (256, 128, 64, 48, 40, 32, 24, 20, 16)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def draw(bold):
    """bold: thicker strokes for the small sizes, where thin lines would vanish."""
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    # The plate: a rounded square, graphite from top to bottom, with a faint warm glow from the top right.
    plate = Image.new('RGBA', (S, S))
    top, bottom = (0x2A, 0x2E, 0x39, 255), (0x0B, 0x0C, 0x10, 255)
    px = plate.load()
    for y in range(S):
        row = lerp(top, bottom, y / (S - 1))
        for x in range(S):
            px[x, y] = row
    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((S * 0.45, -S * 0.45, S * 1.35, S * 0.45), fill=(0xFF, 0x5A, 0x3D, 70))
    plate = Image.alpha_composite(plate, glow.filter(ImageFilter.GaussianBlur(S * 0.12)))
    mask = Image.new('L', (S, S), 0)
    inset = 28 if not bold else 8
    ImageDraw.Draw(mask).rounded_rectangle((inset, inset, S - inset, S - inset), radius=int(S * 0.215), fill=255)
    img.paste(plate, (0, 0), mask)
    d = ImageDraw.Draw(img)
    if not bold:
        d.rounded_rectangle((inset + 5, inset + 5, S - inset - 5, S - inset - 5), radius=int(S * 0.21), outline=(255, 255, 255, 38), width=6)

    # The rev counter: 240 degrees of arc, open at the bottom; the last part is the red line.
    cx, cy, r = S / 2, S * 0.555, S * (0.30 if not bold else 0.315)
    w = int(S * (0.078 if not bold else 0.115))
    start, red, end = 150, 318, 390
    white, warm = (0xEE, 0xF1, 0xF6, 255), (0xFF, 0x5A, 0x3D, 255)
    box = (cx - r, cy - r, cx + r, cy + r)
    d.arc(box, start, red, fill=white, width=w)
    d.arc(box, red, end, fill=warm, width=w)
    for angle, colour in ((start, white), (end, warm)):          # round ends
        a = math.radians(angle)
        x, y = cx + math.cos(a) * (r - w / 2), cy + math.sin(a) * (r - w / 2)
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=colour)
    if not bold:                                                   # ticks inside the arc
        for i in range(0, 9):
            a = math.radians(start + i * (end - start) / 8)
            r0, r1 = r - w - S * 0.03, r - w - S * (0.085 if i % 2 == 0 else 0.06)
            d.line((cx + math.cos(a) * r0, cy + math.sin(a) * r0, cx + math.cos(a) * r1, cy + math.sin(a) * r1),
                   fill=warm if i >= 6 else (0xA9, 0xB1, 0xC2, 255), width=int(S * 0.016))
    # The needle, up and to the right, where the red line begins.
    a = math.radians(322)
    nw = int(S * (0.05 if not bold else 0.085))
    tip = (cx + math.cos(a) * (r - w * 0.5), cy + math.sin(a) * (r - w * 0.5))
    tail = (cx - math.cos(a) * S * 0.05, cy - math.sin(a) * S * 0.05)
    d.line((tail, tip), fill=warm, width=nw)
    for x, y in (tip, tail):
        d.ellipse((x - nw / 2, y - nw / 2, x + nw / 2, y + nw / 2), fill=warm)
    hub = S * (0.062 if not bold else 0.085)
    d.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=warm)
    if not bold:
        d.ellipse((cx - hub * 0.42, cy - hub * 0.42, cx + hub * 0.42, cy + hub * 0.42), fill=(0x16, 0x18, 0x1F, 255))
    return img


def bmp_entry(im):
    w, h = im.size
    pixels = im.tobytes('raw', 'BGRA')
    rows = [pixels[y * w * 4:(y + 1) * w * 4] for y in range(h)]
    xor = b''.join(reversed(rows))                                  # bottom-up
    mask_row = b'\x00' * (((w + 31) // 32) * 4)                     # the alpha channel does the work
    header = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(xor) + len(mask_row) * h, 0, 0, 0, 0)
    return header + xor + mask_row * h


def main():
    out = sys.argv[1]
    large, small = draw(False), draw(True)
    entries = []
    for size in SIZES:
        im = (large if size >= 48 else small).resize((size, size), Image.LANCZOS)
        if size == 256:
            buf = io.BytesIO()
            im.save(buf, 'PNG')
            data = buf.getvalue()
        else:
            data = bmp_entry(im)
        entries.append((size, data))
    with open(out, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(entries)))
        offset = 6 + 16 * len(entries)
        for size, data in entries:
            f.write(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
            offset += len(data)
        for _, data in entries:
            f.write(data)
    if len(sys.argv) > 2:                                           # what it looks like: the large drawing and the small ones
        sheet = Image.new('RGBA', (256 + 16 + 128 + 16 + 64 + 16 + 48 + 16 + 32 + 16 + 16 + 32, 288), (0x30, 0x34, 0x3C, 255))
        x = 16
        for size in (256, 128, 64, 48, 32, 16):
            im = (large if size >= 48 else small).resize((size, size), Image.LANCZOS)
            sheet.alpha_composite(im, (x, 16))
            x += size + 16
        sheet.save(sys.argv[2])
    print('wrote', out, [s for s, _ in entries])


main()
