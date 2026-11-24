#include "meta.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

static void copy_upper(char* dst, size_t cap, const char* src, size_t n) {
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = c;
    }
    dst[n] = 0;
}

static void set_field(char* dst, size_t cap, const char* src, size_t n) {
    if (dst[0]) return;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// ---------- vorbis comments (shared by FLAC & OGG) ----------
static void parse_vorbis_comments(const unsigned char* p, size_t n, TrackMeta* m) {
    if (n < 8) return;
    uint32_t vendor_len;
    memcpy(&vendor_len, p, 4);
    size_t off = 4 + vendor_len;
    if (off + 4 > n) return;
    uint32_t count;
    memcpy(&count, p + off, 4);
    off += 4;
    for (uint32_t i = 0; i < count && off + 4 <= n; i++) {
        uint32_t len;
        memcpy(&len, p + off, 4);
        off += 4;
        if (off + len > n) break;
        const char* s = (const char*)(p + off);
        const char* eq = memchr(s, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - s);
            char key[32];
            copy_upper(key, sizeof(key), s, klen);
            size_t vlen = len - klen - 1;
            if (!strcmp(key, "TITLE")) set_field(m->title, sizeof(m->title), eq + 1, vlen);
            else if (!strcmp(key, "ARTIST") || !strcmp(key, "PERFORMER")) set_field(m->artist, sizeof(m->artist), eq + 1, vlen);
            else if (!strcmp(key, "ALBUM")) set_field(m->album, sizeof(m->album), eq + 1, vlen);
        }
        off += len;
    }
}

// ---------- FLAC ----------
static int parse_flac(const char* path, TrackMeta* m) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[8];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return 0; }
    if (!memcmp(hdr, "ID3", 3)) {
        // skip id3
        unsigned char idh[6];
        if (fread(idh, 1, 6, f) != 6) { fclose(f); return 0; }
        unsigned sz = (idh[2] & 0x7F) << 21 | (idh[3] & 0x7F) << 14 | (idh[4] & 0x7F) << 7 | (idh[5] & 0x7F);
        if (fseek(f, (long)sz, SEEK_CUR) != 0) { fclose(f); return 0; }
        if (fread(hdr, 1, 4, f) != 4) { fclose(f); return 0; }
    }
    if (memcmp(hdr, "fLaC", 4)) { fclose(f); return 0; }
    int got = 0;
    for (;;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        int last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        size_t len = ((size_t)bh[1] << 16) | ((size_t)bh[2] << 8) | bh[3];
        if (len > 16u * 1024u * 1024u) {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        } else if (type == 4 || type == 6) {
            unsigned char* buf = (unsigned char*)malloc(len);
            if (!buf || fread(buf, 1, len, f) != len) { free(buf); break; }
            if (type == 4) {
                parse_vorbis_comments(buf, len, m);
                got = 1;
            } else {
                // PICTURE block
                if (len >= 32) {
                    size_t off = 4; // picture type
                    uint32_t mime_len;
                    memcpy(&mime_len, buf + off, 4); off += 4;
                    if (off + mime_len + 4 <= len) {
                        off += mime_len;
                        uint32_t desc_len;
                        memcpy(&desc_len, buf + off, 4); off += 4;
                        if (off + desc_len + 16 + 4 <= len) {
                            off += desc_len;
                            off += 16; // w,h,depth,ncolors
                            uint32_t data_len;
                            memcpy(&data_len, buf + off, 4); off += 4;
                            if (off + data_len <= len && data_len > 0 && !m->art) {
                                int w, h, comp;
                                m->art = stbi_load_from_memory(buf + off, (int)data_len, &w, &h, &comp, 4);
                                if (m->art) { m->art_w = w; m->art_h = h; got = 1; }
                            }
                        }
                    }
                }
            }
            free(buf);
        } else {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (last) break;
    }
    fclose(f);
    return got;
}

// ---------- ID3v2 ----------
typedef struct {
    unsigned char* body;
    size_t size;
    int ver; // 2,3,4
} Id3;

static int id3_open(const char* path, Id3* id) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) { fclose(f); return 0; }
    id->ver = h[3];
    size_t sz = ((size_t)(h[6] & 0x7F) << 21) | ((size_t)(h[7] & 0x7F) << 14) | ((size_t)(h[8] & 0x7F) << 7) | (size_t)(h[9] & 0x7F);
    if (sz > 16u * 1024u * 1024u) { fclose(f); return 0; }
    id->size = sz;
    id->body = (unsigned char*)malloc(sz ? sz : 1);
    if (!id->body) { fclose(f); return 0; }
    if (fread(id->body, 1, sz, f) != sz) { free(id->body); id->body = NULL; fclose(f); return 0; }
    fclose(f);
    return 1;
}

static void decode_text(const unsigned char* data, size_t n, char* out, size_t cap) {
    if (n < 1) return;
    int enc = data[0];
    const unsigned char* t = data + 1;
    size_t tn = n - 1;
    if (enc == 0) {
        latin1_to_utf8(t, tn, out, cap);
    } else if (enc == 1) {
        if (tn >= 2) {
            int be = 0;
            if (t[0] == 0xFF && t[1] == 0xFE) { t += 2; tn -= 2; }
            else if (t[0] == 0xFE && t[1] == 0xFF) { be = 1; t += 2; tn -= 2; }
            uint16_t* u16 = (uint16_t*)malloc(tn + 2);
            if (!u16) return;
            for (size_t i = 0; i < tn / 2; i++) {
                uint16_t v = (uint16_t)(t[i*2] | (t[i*2+1] << 8));
                if (be) v = (uint16_t)((v >> 8) | (v << 8));
                u16[i] = v;
            }
            my_utf16_to_utf8(u16, tn / 2, out, cap);
            free(u16);
        }
    } else if (enc == 2) {
        // UTF-16BE no BOM
        uint16_t* u16 = (uint16_t*)malloc(tn + 2);
        if (!u16) return;
        for (size_t i = 0; i < tn / 2; i++)
            u16[i] = (uint16_t)((t[i*2] << 8) | t[i*2+1]);
        my_utf16_to_utf8(u16, tn / 2, out, cap);
        free(u16);
    } else {
        // UTF-8, maybe BOM
        if (tn >= 3 && t[0] == 0xEF && t[1] == 0xBB && t[2] == 0xBF) { t += 3; tn -= 3; }
        if (tn >= cap) tn = cap - 1;
        // stop at first NUL
        const unsigned char* z = memchr(t, 0, tn);
        if (z) tn = (size_t)(z - t);
        memcpy(out, t, tn);
        out[tn] = 0;
    }
}

static int id3_parse(Id3* id, TrackMeta* m) {
    int got = 0;
    size_t pos = 0;
    while (id->ver == 2 ? (pos + 6 <= id->size) : (pos + 10 <= id->size)) {
        char fid[5] = {0};
        size_t fsize;
        if (id->ver == 2) {
            memcpy(fid, id->body + pos, 3);
            fsize = ((size_t)id->body[pos+3] << 16) | ((size_t)id->body[pos+4] << 8) | id->body[pos+5];
            pos += 6;
        } else {
            memcpy(fid, id->body + pos, 4);
            if (id->ver == 4) fsize = ((size_t)(id->body[pos+4] & 0x7F) << 21) | ((size_t)(id->body[pos+5] & 0x7F) << 14) | ((size_t)(id->body[pos+6] & 0x7F) << 7) | (size_t)(id->body[pos+7] & 0x7F);
            else fsize = ((size_t)id->body[pos+4] << 24) | ((size_t)id->body[pos+5] << 16) | ((size_t)id->body[pos+6] << 8) | (size_t)id->body[pos+7];
            pos += 10;
        }
        if (fid[0] == 0) break; // padding
        if (pos + fsize > id->size) break;
        if (fsize == 0) continue;
        unsigned char* data = id->body + pos;
        int is_title = !strcmp(fid, "TIT2") || !strcmp(fid, "TT2");
        int is_artist = !strcmp(fid, "TPE1") || !strcmp(fid, "TP1");
        int is_album = !strcmp(fid, "TALB") || !strcmp(fid, "TAL");
        int is_pic = !strcmp(fid, "APIC") || !strcmp(fid, "PIC");
        if (is_title) {
            decode_text(data, fsize, m->title, sizeof(m->title));
            got = 1;
        } else if (is_artist) {
            decode_text(data, fsize, m->artist, sizeof(m->artist));
            got = 1;
        } else if (is_album) {
            decode_text(data, fsize, m->album, sizeof(m->album));
            got = 1;
        } else if (is_pic && !m->art && fsize > 8) {
            int enc = data[0];
            const unsigned char* p = data + 1;
            const unsigned char* end = data + fsize;
            if (id->ver == 2) {
                // PIC: format(3) type(1) desc(z)
                if (p + 4 > end) { pos += fsize; continue; }
                p += 3 + 1;
            } else {
                // APIC: mime(z) type(1) desc(z)
                const unsigned char* z = memchr(p, 0, (size_t)(end - p));
                if (!z || z + 2 > end) { pos += fsize; continue; }
                p = z + 1 + 1; // skip mime + picture type
            }
            // skip description (NUL-terminated; 2 bytes if UTF-16)
            if (enc == 1 || enc == 2) {
                while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p += 2;
                p += 2;
            } else {
                while (p < end && *p) p++;
                p++;
            }
            if (p < end && (size_t)(end - p) > 16) {
                int w, h, comp;
                m->art = stbi_load_from_memory(p, (int)(end - p), &w, &h, &comp, 4);
                if (m->art) { m->art_w = w; m->art_h = h; got = 1; }
            }
        }
        pos += fsize;
    }
    return got;
}

// ---------- OGG ----------
static int parse_ogg(const char* path, TrackMeta* m) {
    int err;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) return 0;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        const char* eq = strchr(s, '=');
        if (!eq) continue;
        char key[32];
        copy_upper(key, sizeof(key), s, (size_t)(eq - s));
        if (!strcmp(key, "TITLE")) set_field(m->title, sizeof(m->title), eq + 1, strlen(eq + 1));
        else if (!strcmp(key, "ARTIST")) set_field(m->artist, sizeof(m->artist), eq + 1, strlen(eq + 1));
        else if (!strcmp(key, "ALBUM")) set_field(m->album, sizeof(m->album), eq + 1, strlen(eq + 1));
    }
    stb_vorbis_close(v);
    return 1;
}

// ---------- 兜底：在原始字节里直接找内嵌 JPEG/PNG ----------
// 很多 MP3 的 APIC 帧格式不标准（描述字段没有 NUL、编码标记错误等），
// 标准标签解析会失败。这里直接在文件头部字节里搜 JPEG/PNG 签名并解码。
static unsigned char* find_image_in_mem(const unsigned char* data, size_t len, int* w, int* h) {
    static const unsigned char png_sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (!data || len < 16) return NULL;
    int tried = 0;
    // 限制尝试次数，避免被音频数据里的大量 0xFF 拖慢
    for (size_t i = 0; i + 16 <= len && tried < 12; i++) {
        int is_png = (i + 8 <= len && !memcmp(data + i, png_sig, 8));
        int is_jpg = (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF);
        if (!is_png && !is_jpg) continue;
        tried++;
        // stb_image 会在 EOI / IEND 处自行停止，尾部多余字节无妨
        int iw = 0, ih = 0;
        unsigned char* img = stbi_load_from_memory(data + i, (int)(len - i), &iw, &ih, NULL, 4);
        if (img && iw > 8 && ih > 8) {
            *w = iw; *h = ih;
            return img;
        }
        if (img) stbi_image_free(img);
    }
    return NULL;
}

// 只读取文件开头 2MB：ID3/FLAC 元数据与内嵌封面几乎都在这一区间，
// 避免为一张缩略图把整首歌读进内存。
#define ART_SCAN_LIMIT (2u * 1024u * 1024u)

static void fallback_art(const char* path, TrackMeta* m, size_t scan_limit) {
    if (m->art) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    unsigned char* buf = (unsigned char*)malloc(scan_limit);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, scan_limit, f);
    fclose(f);
    if (n > 16) {
        int w = 0, h = 0;
        unsigned char* img = find_image_in_mem(buf, n, &w, &h);
        if (img) {
            m->art = img;
            m->art_w = w;
            m->art_h = h;
        }
    }
    free(buf);
}

static int meta_load_impl(const char* path, TrackMeta* m, size_t scan_limit) {
    memset(m, 0, sizeof(*m));
    int got = 0;
    if (ci_ends_with(path, ".flac")) got = parse_flac(path, m);
    else if (ci_ends_with(path, ".mp3")) {
        Id3 id;
        if (id3_open(path, &id)) {
            got = id3_parse(&id, m);
            free(id.body);
        }
    } else if (ci_ends_with(path, ".ogg")) got = parse_ogg(path, m);

    // 标签解析没有拿到封面时，用二进制签名兜底
    fallback_art(path, m, scan_limit);
    if (m->art) got = 1;

    if (!m->title[0]) basename_no_ext(path, m->title, sizeof(m->title));
    return got;
}

int meta_load(const char* path, TrackMeta* m) {
    return meta_load_impl(path, m, ART_SCAN_LIMIT);
}

int meta_load_quick(const char* path, TrackMeta* m) {
    return meta_load_impl(path, m, 256u * 1024u);
}

void meta_release_art(TrackMeta* m) {
    if (m->art) {
        stbi_image_free(m->art);
        m->art = NULL;
        m->art_w = 0;
        m->art_h = 0;
    }
}

void meta_clear(TrackMeta* m) {
    meta_release_art(m);
    memset(m, 0, sizeof(*m));
}
