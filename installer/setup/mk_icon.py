# 生成清晰的小尺寸图标：16/24 用简化版（实心圆角 + 白色电视），大尺寸用带霓虹环的版本
from PIL import Image, ImageDraw, ImageFilter
import math

SRC = r"D:\DSH-Workplace\projector\sender-pc\ui\logo_light.png"   # 1280 母版（淡紫渐变底 + 霓虹环）
OUT = r"D:\DSH-Workplace\projector\installer\payload\logo.ico"

def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    return m

def simple_icon(px):
    """16/24 用：渐变圆角底 + 白色电视线条，避免霓虹环糊成一团"""
    S = px * 8
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    grad = Image.new("RGB", (S, S))
    p = grad.load()
    c1, c2 = (0x5B, 0x3E, 0xE0), (0xA6, 0x5C, 0xF0)
    for y in range(S):
        for x in range(S):
            t = (x + y) / (2.0 * S)
            p[x, y] = (int(c1[0] + (c2[0] - c1[0]) * t),
                       int(c1[1] + (c2[1] - c1[1]) * t),
                       int(c1[2] + (c2[2] - c1[2]) * t))
    base.paste(grad, (0, 0), rounded_mask(S, int(S * 0.24)))
    d = ImageDraw.Draw(base)
    w, h = S * 0.56, S * 0.36
    lw = max(2, int(S * 0.075))
    x0, y0 = (S - w) / 2, (S - h) / 2 - S * 0.02
    d.rounded_rectangle([x0, y0, x0 + w, y0 + h], radius=S * 0.05, outline=(255, 255, 255, 255), width=lw)
    leg = max(2, int(lw * 0.8))
    d.line([x0 + w * 0.28, y0 + h, x0 + w * 0.20, y0 + h + S * 0.11], fill=(255, 255, 255, 255), width=leg)
    d.line([x0 + w * 0.72, y0 + h, x0 + w * 0.80, y0 + h + S * 0.11], fill=(255, 255, 255, 255), width=leg)
    return base.resize((px, px), Image.LANCZOS)

def big_icon(px):
    src = Image.open(SRC).convert("RGBA")
    return src.resize((px, px), Image.LANCZOS)

frames = []
for px in (16, 24):
    frames.append(simple_icon(px))
for px in (32, 48, 64, 128, 256):
    frames.append(big_icon(px))

frames[-1].save(OUT, format="ICO", sizes=[(f.width, f.height) for f in frames])
print("written", OUT)

# 预览一张 16/24/32/48 的放大对照
prev = Image.new("RGBA", (4 * 96 + 40, 110), (245, 246, 248, 255))
x = 10
for f in frames[:4]:
    prev.alpha_composite(f.resize((96, 96), Image.NEAREST), (x, 7))
    x += 100
prev.convert("RGB").save(r"D:\DSH-Workplace\projector\installer\setup\icon_preview.png")
print("preview ok")
