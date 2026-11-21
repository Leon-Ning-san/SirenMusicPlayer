    .section .rodata
    .balign 16
    .global poster_start
poster_start:
    .incbin "../deps/poster.rgba"
    .global poster_end
poster_end:
