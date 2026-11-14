"""从唯一源 logo.png 生成各处图标，保证品牌一致。

源：sender-pc/ui/logo.png（1280x1280，紫色圆角方 + 霓虹环 + 电视线稿）
输出：
  receiver-tv/res/mipmap-xhdpi/logo.png   512x512 正方形（裁掉白边，电视桌面图标）
  installer/payload/logo.ico              多尺寸 ico（安装包/程序图标）
  sender-pc/ui/logo.ico                   多尺寸 ico
  installer/setup/icon_preview.png       256x256 预览
"""
import os
from PIL import Image

ROOT = r"D:\DSH-Workplace\XingLianCast"
SRC = os.path.join(ROOT, "sender-pc", "ui", "logo.png")


def trim_white(img: Image.Image, tol: int = 8) -> Image.Image:
    """裁掉四周接近纯白的边（保留轻微抗锯齿）。"""
    rgba = img.convert("RGBA")
    px = rgba.load()
    w, h = rgba.size
    left, top, right, bottom = w, h, 0, 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a > 10 and not (r > 255 - tol and g > 255 - tol and b > 255 - tol):
                if x < left:
                    left = x
                if x > right:
                    right = x
                if y < top:
                    top = y
                if y > bottom:
                    bottom = y
    if right <= left or bottom <= top:
        return rgba
    return rgba.crop((left, top, right + 1, bottom + 1))


def square(img: Image.Image) -> Image.Image:
    """居中补成正方形（不拉伸）。"""
    w, h = img.size
    s = max(w, h)
    canvas = Image.new("RGBA", (s, s), (255, 255, 255, 0))
    canvas.paste(img, ((s - w) // 2, (s - h) // 2), img)
    return canvas


def save_png(img: Image.Image, path: str, size: int):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out = img.resize((size, size), Image.LANCZOS)
    out.save(path, "PNG", optimize=True)
    print("PNG %-58s %dx%d" % (path.replace(ROOT, ""), size, size))


def save_ico(img: Image.Image, path: str):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(path, "ICO", sizes=sizes)
    print("ICO %-58s %d sizes" % (path.replace(ROOT, ""), len(sizes)))


def main():
    src = Image.open(SRC).convert("RGBA")
    art = square(trim_white(src))
    print("裁切后: %dx%d" % art.size)

    save_png(art, os.path.join(ROOT, "receiver-tv", "res", "mipmap-xhdpi", "logo.png"), 512)
    save_ico(art, os.path.join(ROOT, "installer", "payload", "logo.ico"))
    save_ico(art, os.path.join(ROOT, "sender-pc", "ui", "logo.ico"))
    save_png(art, os.path.join(ROOT, "installer", "setup", "icon_preview.png"), 256)
    # 电视端 App 内大图保持 1280（已有），这里再导一份以防需要
    save_png(art, os.path.join(ROOT, "sender-pc", "ui", "logo_light.png"), 512)


if __name__ == "__main__":
    main()
