import struct, sys

nacp = sys.argv[1]
d = bytearray(open(nacp, "rb").read())

def write_slot(base, text):
    enc = (text + "\0").encode("utf-16le")
    for i in range(16):
        off = base + i * 0x200
        d[off:off + 0x200] = b"\0" * 0x200
        d[off:off + len(enc)] = enc

write_slot(0x0000, "Siren")   # 16 个语言标题
write_slot(0x2000, "Siren")   # 16 个语言发行商
open(nacp, "wb").write(d)
print("NACP UTF-16LE 已修正")
