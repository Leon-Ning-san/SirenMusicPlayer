#ifdef __APPLE__
#define SYM(x) _##x
.const_data
#else
#define SYM(x) x
.section .rodata
#endif

.balign 16
.globl SYM(font_ttf_start)
SYM(font_ttf_start):
    .incbin "deps/NotoSansSC.ttf"
.globl SYM(font_ttf_end)
SYM(font_ttf_end):

.balign 16
.globl SYM(poster_start)
SYM(poster_start):
    .incbin "deps/poster.rgba"
.globl SYM(poster_end)
SYM(poster_end):
