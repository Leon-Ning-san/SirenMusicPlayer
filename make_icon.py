from PIL import Image, ImageDraw

S = 256
img = Image.new("RGB", (S, S), (17, 19, 22))
d = ImageDraw.Draw(img)

# 第一版图标：深色背景 + 青色圆角方块 + 深色双音符
d.rounded_rectangle([24, 24, 232, 232], radius=48, fill=(82, 214, 192))

note = (10, 38, 34)
d.ellipse([82, 150, 128, 190], fill=note)
d.ellipse([146, 134, 192, 174], fill=note)
d.rectangle([116, 74, 130, 170], fill=note)
d.rectangle([180, 58, 194, 154], fill=note)
d.polygon([(116, 74), (194, 58), (194, 78), (116, 94)], fill=note)

img.save(r"C:\Users\78130\WorkBuddy AI\2026-11-17-16-57-58\musicplayer\icon.jpg", quality=95, subsampling=0)
print("第一版青色音符图标已生成")
