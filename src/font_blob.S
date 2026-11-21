    .section .rodata
    .balign 16
    .global font_ttf_start
font_ttf_start:
    .incbin "../deps/NotoSansSC.ttf"
    .global font_ttf_end
font_ttf_end:
