#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// UTF-16 (LE or BE, with or without BOM) -> UTF-8. Returns bytes written (excl. NUL).
size_t my_utf16_to_utf8(const uint16_t* in, size_t in_units, char* out, size_t out_cap);
// Latin-1 (ISO-8859-1) -> UTF-8
size_t latin1_to_utf8(const uint8_t* in, size_t n, char* out, size_t out_cap);
// Check if byte buffer is valid UTF-8
int is_valid_utf8(const uint8_t* s, size_t n);
// Copy UTF-8 string safely up to min(cap-1, max_bytes) without splitting multi-byte codepoints
void utf8_copy_safe(char* dst, size_t cap, const char* src, size_t max_bytes);
// Heuristic: 1 if byte stream is predominantly valid UTF-8 (tolerate rare stray bytes)
int looks_like_utf8_text(const uint8_t* s, size_t n);
// Sanitize UTF-8 byte buffer into out (replacing malformed bytes, normalizing NBSP to space)
size_t sanitize_utf8_text(const uint8_t* in, size_t n, char* out, size_t out_cap);
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

#ifdef __cplusplus
}
#endif
