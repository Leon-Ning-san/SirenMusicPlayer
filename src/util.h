#pragma once
#include <stdint.h>
#include <stddef.h>

// UTF-16 (LE or BE, with or without BOM) -> UTF-8. Returns bytes written (excl. NUL).
size_t my_utf16_to_utf8(const uint16_t* in, size_t in_units, char* out, size_t out_cap);
// Latin-1 (ISO-8859-1) -> UTF-8
size_t latin1_to_utf8(const uint8_t* in, size_t n, char* out, size_t out_cap);
// Check if byte buffer is valid UTF-8
int is_valid_utf8(const uint8_t* s, size_t n);
// GBK/GB2312 -> UTF-8
size_t gbk_to_utf8(const uint8_t* in, size_t n, char* out, size_t out_cap);
// Auto decode UTF-8 (if valid) or GBK -> UTF-8
size_t text_to_utf8_auto(const uint8_t* in, size_t n, char* out, size_t out_cap);
// count UTF-8 code points
int utf8_count(const char* s);
// case-insensitive ends with
int ci_ends_with(const char* s, const char* suf);
const char* path_last_sep(const char* p);
const char* path_basename(const char* p);
// copy basename without extension into out
void basename_no_ext(const char* path, char* out, size_t cap);
// format "m:ss" or "mm:ss"
void fmt_time(double sec, char* out, size_t cap);
