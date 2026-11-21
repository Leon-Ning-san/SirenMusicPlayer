from PIL import Image
from collections import deque

src = r"C:\Users\78130\.workbuddy-ai\clipboard-images\clipboard-2026-11-17T11-40-50-887Z-9a608fde.jpg"
base = r"C:\Users\78130\WorkBuddy AI\2026-11-17-16-57-58\musicplayer"

im = Image.open(src).convert("RGB")
W, H = im.size  # 1440 x 1920

# 更聚焦脸部的方形裁切，并把脸部放到画面中央
half = 420
cx, cy = int(W * 0.72), int(H * 0.32)
left = max(0, min(W - 2 * half, cx - half))
top = max(0, min(H - 2 * half, cy - half))
box = (left, top, left + 2 * half, top + 2 * half)
crop = im.crop(box).resize((320, 320), Image.LANCZOS)


def fill_white_edges(image, dark=(16, 18, 21), threshold=215):
    """把与边缘相连的近白背景填充为深色，保留人物与内部白色故障条。"""
    px = image.load()
    w, h = image.size
    seen = [[False] * w for _ in range(h)]
    q = deque()

    def is_bg(x, y):
        r, g, b = px[x, y]
        return min(r, g, b) > threshold and (max(r, g, b) - min(r, g, b)) < 25

    for x in range(w):
        for y in (0, h - 1):
            if is_bg(x, y) and not seen[y][x]:
                seen[y][x] = True
                q.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if is_bg(x, y) and not seen[y][x]:
                seen[y][x] = True
                q.append((x, y))

    while q:
        x, y = q.popleft()
        px[x, y] = dark
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and is_bg(nx, ny):
                seen[ny][nx] = True
                q.append((nx, ny))
    return image


crop = fill_white_edges(crop)

# hbmenu 图标：256x256 JPEG
icon = crop.resize((256, 256), Image.LANCZOS)
icon.save(base + r"\icon.jpg", quality=94, subsampling=0)
print("icon:", box, "->", icon.size)

# 程序内嵌海报：320x320 原始 RGBA（字节序 R,G,B,A 与 COL 宏一致）
cover = crop.convert("RGBA")
open(base + r"\deps\poster.rgba", "wb").write(cover.tobytes())
print("poster.rgba:", 320 * 320 * 4, "bytes")
