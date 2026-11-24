#include "meta.h"
#include "util.h"
#include "plat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

static uint32_t read_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t read_syncsafe32(const unsigned char* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (uint32_t)(p[3] & 0x7F);
}

static size_t deunsync_copy(const unsigned char* src, size_t n, unsigned char* dst) {
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        dst[j++] = src[i];
        if (src[i] == 0xFF && i + 1 < n && src[i + 1] == 0x00) i++;
    }
    return j;
}

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
    if (dst[0] || !src || n == 0) return;
    text_to_utf8_auto((const uint8_t*)src, n, dst, cap);
}

// 解析 FLAC 标准大端序 METADATA_BLOCK_PICTURE 二进制块
static void decode_flac_picture_buf(const unsigned char* buf, size_t len, TrackMeta* m) {
    if (!buf || len < 32 || m->art) return;
    size_t off = 4; // skip picture type (4-byte BE)
    uint32_t mime_len = read_be32(buf + off); off += 4;
    if (mime_len > 256 || off + mime_len + 4 > len) return;
    off += mime_len;
    uint32_t desc_len = read_be32(buf + off); off += 4;
    if (desc_len > len || off + desc_len + 20 > len) return;
    off += desc_len;
    off += 16; // width, height, depth, colors (4 x 4-byte BE)
    uint32_t data_len = read_be32(buf + off); off += 4;
    if (data_len == 0 || data_len > len || off + data_len > len) return;
    int w = 0, h = 0, comp = 0;
    unsigned char* img = stbi_load_from_memory(buf + off, (int)data_len, &w, &h, &comp, 4);
    if (img && w > 8 && h > 8) {
        m->art = img;
        m->art_w = w;
        m->art_h = h;
    } else if (img) {
        stbi_image_free(img);
    }
}

// Base64 解码（用于 OGG Vorbis METADATA_BLOCK_PICTURE）
static unsigned char* base64_decode_alloc(const char* s, size_t slen, size_t* out_len) {
    if (!s || slen < 16) return NULL;
    size_t cap = (slen * 3) / 4 + 8;
    unsigned char* out = (unsigned char*)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t val = 0;
    int bits = -8;
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        int d = -1;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else if (c == '=') break;
        else continue;
        val = (val << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 0) {
            out[o++] = (unsigned char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    *out_len = o;
    return out;
}

// ---------- vorbis comments (shared by FLAC & OGG) ----------
static void parse_vorbis_comments(const unsigned char* p, size_t n, TrackMeta* m) {
    if (n < 8) return;
    uint32_t vendor_len;
    memcpy(&vendor_len, p, 4);
    size_t off = 4 + (size_t)vendor_len;
    if (off + 4 > n) return;
    uint32_t count;
    memcpy(&count, p + off, 4);
    off += 4;
    for (uint32_t i = 0; i < count && off + 4 <= n; i++) {
        uint32_t len;
        memcpy(&len, p + off, 4);
        off += 4;
        if (len > n || off + len > n) break;
        const char* s = (const char*)(p + off);
        const char* eq = (const char*)memchr(s, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - s);
            char key[40];
            copy_upper(key, sizeof(key), s, klen);
            size_t vlen = len - klen - 1;
            if (!strcmp(key, "TITLE")) set_field(m->title, sizeof(m->title), eq + 1, vlen);
            else if (!strcmp(key, "ARTIST") || !strcmp(key, "PERFORMER") || !strcmp(key, "ALBUMARTIST"))
                set_field(m->artist, sizeof(m->artist), eq + 1, vlen);
            else if (!strcmp(key, "ALBUM")) set_field(m->album, sizeof(m->album), eq + 1, vlen);
            else if (!strcmp(key, "METADATA_BLOCK_PICTURE") && !m->art && vlen < 8u * 1024u * 1024u) {
                size_t raw_len = 0;
                unsigned char* raw = base64_decode_alloc(eq + 1, vlen, &raw_len);
                if (raw) {
                    decode_flac_picture_buf(raw, raw_len, m);
                    free(raw);
                }
            }
        }
        off += len;
    }
}

// ---------- ID3v2 ----------
typedef struct {
    unsigned char* body;
    size_t size;
    int ver; // 2,3,4
    int flags;
} Id3;

static int id3_open(const char* path, Id3* id) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) { fclose(f); return 0; }
    id->ver = h[3];
    id->flags = h[5];
    size_t sz = read_syncsafe32(h + 6);
    if (sz == 0 || sz > 16u * 1024u * 1024u) { fclose(f); return 0; }
    unsigned char* raw = (unsigned char*)malloc(sz);
    if (!raw) { fclose(f); return 0; }
    if (fread(raw, 1, sz, f) != sz) { free(raw); fclose(f); return 0; }
    fclose(f);

    if (id->flags & 0x80) {
        id->body = (unsigned char*)malloc(sz);
        if (!id->body) { free(raw); return 0; }
        id->size = deunsync_copy(raw, sz, id->body);
        free(raw);
    } else {
        id->body = raw;
        id->size = sz;
    }
    return 1;
}

static void decode_text(const unsigned char* data, size_t n, char* out, size_t cap) {
    if (n < 1 || !out || cap == 0 || out[0]) return;
    int enc = data[0];
    const unsigned char* t = data + 1;
    size_t tn = n - 1;
    if (enc == 0) {
        text_to_utf8_auto(t, tn, out, cap);
    } else if (enc == 1) {
        if (tn >= 2) {
            int be = 0;
            if (t[0] == 0xFF && t[1] == 0xFE) { t += 2; tn -= 2; }
            else if (t[0] == 0xFE && t[1] == 0xFF) { be = 1; t += 2; tn -= 2; }
            size_t units = 0;
            while (units + 1 <= tn / 2) {
                uint16_t lo = t[units * 2], hi = t[units * 2 + 1];
                if (lo == 0 && hi == 0) break;
                units++;
            }
            uint16_t* u16 = (uint16_t*)malloc(units * 2 + 2);
            if (!u16) return;
            for (size_t i = 0; i < units; i++) {
                uint16_t v = (uint16_t)(t[i * 2] | (t[i * 2 + 1] << 8));
                if (be) v = (uint16_t)((v >> 8) | (v << 8));
                u16[i] = v;
            }
            my_utf16_to_utf8(u16, units, out, cap);
            free(u16);
        }
    } else if (enc == 2) {
        size_t units = 0;
        while (units + 1 <= tn / 2) {
            if (t[units * 2] == 0 && t[units * 2 + 1] == 0) break;
            units++;
        }
        uint16_t* u16 = (uint16_t*)malloc(units * 2 + 2);
        if (!u16) return;
        for (size_t i = 0; i < units; i++)
            u16[i] = (uint16_t)((t[i * 2] << 8) | t[i * 2 + 1]);
        my_utf16_to_utf8(u16, units, out, cap);
        free(u16);
    } else {
        text_to_utf8_auto(t, tn, out, cap);
    }
}

static int valid_id3_fid4(const unsigned char* p) {
    for (int i = 0; i < 4; i++) {
        unsigned char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
    }
    return 1;
}

static void decode_apic_payload(int ver, const unsigned char* data, size_t fsize, TrackMeta* m) {
    if (fsize <= 8 || m->art) return;
    int enc = data[0];
    const unsigned char* p = data + 1;
    const unsigned char* end = data + fsize;
    if (ver == 2) {
        if (p + 4 > end) return;
        p += 3 + 1;
    } else {
        const unsigned char* z = (const unsigned char*)memchr(p, 0, (size_t)(end - p));
        if (!z || z + 2 > end) return;
        p = z + 1 + 1;
    }
    if (enc == 1 || enc == 2) {
        while (p + 1 < end && !(p[0] == 0 && p[1] == 0)) p += 2;
        if (p + 1 < end) p += 2;
    } else {
        while (p < end && *p) p++;
        if (p < end) p++;
    }
    // 如果描述字段不规范，向后寻找真实 JPEG / PNG 魔数起点
    static const unsigned char png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    const unsigned char* img_p = NULL;
    for (const unsigned char* q = data + 1; q + 8 < end; q++) {
        if ((q[0] == 0xFF && q[1] == 0xD8 && q[2] == 0xFF) || !memcmp(q, png_sig, 8)) {
            img_p = q;
            break;
        }
    }
    if (!img_p && p < end) img_p = p;
    if (img_p && img_p < end && (size_t)(end - img_p) > 16) {
        int w = 0, h = 0, comp = 0;
        unsigned char* img = stbi_load_from_memory(img_p, (int)(end - img_p), &w, &h, &comp, 4);
        if (img && w > 8 && h > 8) {
            m->art = img;
            m->art_w = w;
            m->art_h = h;
        } else if (img) {
            stbi_image_free(img);
        }
    }
}

static int id3_parse(Id3* id, TrackMeta* m) {
    int got = 0;
    size_t pos = 0;

    // 跳过 ID3v2.3 / ID3v2.4 Extended Header
    if ((id->flags & 0x40) && pos + 4 <= id->size) {
        size_t ext_sz = (id->ver == 4) ? read_syncsafe32(id->body + pos) : read_be32(id->body + pos);
        if (id->ver == 3) ext_sz += 4;
        if (ext_sz >= 4 && pos + ext_sz <= id->size) pos += ext_sz;
    }

    while (id->ver == 2 ? (pos + 6 <= id->size) : (pos + 10 <= id->size)) {
        char fid[5] = {0};
        size_t fsize = 0;
        unsigned char flag2 = 0;
        if (id->ver == 2) {
            memcpy(fid, id->body + pos, 3);
            fsize = ((size_t)id->body[pos + 3] << 16) | ((size_t)id->body[pos + 4] << 8) | id->body[pos + 5];
            pos += 6;
        } else {
            if (!valid_id3_fid4(id->body + pos)) break;
            memcpy(fid, id->body + pos, 4);
            flag2 = id->body[pos + 9];
            if (id->ver == 4) {
                size_t ss = read_syncsafe32(id->body + pos + 4);
                size_t be = read_be32(id->body + pos + 4);
                fsize = ss;
                if (ss != be && pos + 10 + ss < id->size && !valid_id3_fid4(id->body + pos + 10 + ss) &&
                    id->body[pos + 10 + ss] != 0 && pos + 10 + be <= id->size &&
                    (pos + 10 + be == id->size || id->body[pos + 10 + be] == 0 || valid_id3_fid4(id->body + pos + 10 + be))) {
                    fsize = be;
                }
            } else {
                fsize = read_be32(id->body + pos + 4);
            }
            pos += 10;
        }
        if (fid[0] == 0 || pos + fsize > id->size) break;
        if (fsize == 0) continue;

        const unsigned char* raw_data = id->body + pos;
        size_t data_len = fsize;
        unsigned char* unsync_buf = NULL;

        if (id->ver == 4) {
            if (flag2 & 0x40) { if (data_len > 1) { raw_data++; data_len--; } }
            if (flag2 & 0x01) { if (data_len > 4) { raw_data += 4; data_len -= 4; } }
            if ((flag2 & 0x02) && !(id->flags & 0x80) && data_len > 0) {
                unsync_buf = (unsigned char*)malloc(data_len);
                if (unsync_buf) {
                    data_len = deunsync_copy(raw_data, data_len, unsync_buf);
                    raw_data = unsync_buf;
                }
            }
        }

        int is_title = !strcmp(fid, "TIT2") || !strcmp(fid, "TT2");
        int is_artist = !strcmp(fid, "TPE1") || !strcmp(fid, "TP1") || (!m->artist[0] && !strcmp(fid, "TPE2"));
        int is_album = !strcmp(fid, "TALB") || !strcmp(fid, "TAL");
        int is_pic = !strcmp(fid, "APIC") || !strcmp(fid, "PIC");

        if (is_title) {
            decode_text(raw_data, data_len, m->title, sizeof(m->title));
            got = 1;
        } else if (is_artist) {
            decode_text(raw_data, data_len, m->artist, sizeof(m->artist));
            got = 1;
        } else if (is_album) {
            decode_text(raw_data, data_len, m->album, sizeof(m->album));
            got = 1;
        } else if (is_pic && !m->art) {
            decode_apic_payload(id->ver, raw_data, data_len, m);
            if (m->art) got = 1;
        }

        if (unsync_buf) free(unsync_buf);
        pos += fsize;
    }
    return got;
}

// ---------- FLAC ----------
static int parse_flac(const char* path, TrackMeta* m) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[8];
    if (fread(hdr, 1, 4, f) != 4) { fclose(f); return 0; }
    int has_id3 = 0;
    if (!memcmp(hdr, "ID3", 3)) {
        has_id3 = 1;
        unsigned char idh[6];
        if (fread(idh, 1, 6, f) != 6) { fclose(f); return 0; }
        unsigned sz = read_syncsafe32(idh + 2);
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
            unsigned char* buf = (unsigned char*)malloc(len ? len : 1);
            if (!buf || fread(buf, 1, len, f) != len) { free(buf); break; }
            if (type == 4) {
                parse_vorbis_comments(buf, len, m);
                got = 1;
            } else if (type == 6 && !m->art) {
                decode_flac_picture_buf(buf, len, m);
                if (m->art) got = 1;
            }
            free(buf);
        } else {
            if (fseek(f, (long)len, SEEK_CUR) != 0) break;
        }
        if (last) break;
    }
    fclose(f);
    if (has_id3 && (!m->art || !m->title[0])) {
        Id3 id;
        if (id3_open(path, &id)) {
            if (id3_parse(&id, m)) got = 1;
            free(id.body);
        }
    }
    return got;
}

// ---------- OGG ----------
static int parse_ogg(const char* path, TrackMeta* m) {
    int err = 0;
    FILE* fp = plat_fopen(path, "rb");
    stb_vorbis* v = fp ? stb_vorbis_open_file(fp, 1, &err, NULL) : NULL;
    if (!v && fp) fclose(fp);
    if (!v) return 0;
    stb_vorbis_comment c = stb_vorbis_get_comment(v);
    for (int i = 0; i < c.comment_list_length; i++) {
        const char* s = c.comment_list[i];
        const char* eq = strchr(s, '=');
        if (!eq) continue;
        char key[40];
        copy_upper(key, sizeof(key), s, (size_t)(eq - s));
        size_t vlen = strlen(eq + 1);
        if (!strcmp(key, "TITLE")) set_field(m->title, sizeof(m->title), eq + 1, vlen);
        else if (!strcmp(key, "ARTIST") || !strcmp(key, "PERFORMER"))
            set_field(m->artist, sizeof(m->artist), eq + 1, vlen);
        else if (!strcmp(key, "ALBUM")) set_field(m->album, sizeof(m->album), eq + 1, vlen);
        else if (!strcmp(key, "METADATA_BLOCK_PICTURE") && !m->art && vlen < 8u * 1024u * 1024u) {
            size_t raw_len = 0;
            unsigned char* raw = base64_decode_alloc(eq + 1, vlen, &raw_len);
            if (raw) {
                decode_flac_picture_buf(raw, raw_len, m);
                free(raw);
            }
        }
    }
    stb_vorbis_close(v);
    return 1;
}

// ---------- 同目录外置封面图片兜底 (<stem>.jpg/png, cover.jpg/png, folder.jpg) ----------
static void try_external_cover(const char* media_path, TrackMeta* m) {
    if (m->art || !media_path) return;
    char dir[520] = {0}, stem[256] = {0};
    const char* slash = path_last_sep(media_path);
    if (slash) {
        size_t dlen = (size_t)(slash - media_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, media_path, dlen);
        dir[dlen] = 0;
        while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) dir[--dlen] = 0;
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    basename_no_ext(media_path, stem, sizeof(stem));

    char cands[7][640];
    snprintf(cands[0], sizeof(cands[0]), "%s/%s.jpg", dir, stem);
    snprintf(cands[1], sizeof(cands[1]), "%s/%s.png", dir, stem);
    snprintf(cands[2], sizeof(cands[2]), "%s/%s.jpeg", dir, stem);
    snprintf(cands[3], sizeof(cands[3]), "%s/cover.jpg", dir);
    snprintf(cands[4], sizeof(cands[4]), "%s/cover.png", dir);
    snprintf(cands[5], sizeof(cands[5]), "%s/folder.jpg", dir);
    snprintf(cands[6], sizeof(cands[6]), "%s/front.jpg", dir);

    for (int i = 0; i < 7; i++) {
        FILE* f = plat_fopen(cands[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 32 && sz <= 8 * 1024 * 1024) {
            unsigned char* buf = (unsigned char*)malloc((size_t)sz);
            if (buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                int w = 0, h = 0, comp = 0;
                unsigned char* img = stbi_load_from_memory(buf, (int)sz, &w, &h, &comp, 4);
                if (img && w > 16 && h > 16) {
                    m->art = img;
                    m->art_w = w;
                    m->art_h = h;
                    free(buf);
                    fclose(f);
                    return;
                }
                if (img) stbi_image_free(img);
            }
            free(buf);
        }
        fclose(f);
    }
}

// ---------- 兜底：在原始字节里直接找内嵌 JPEG/PNG ----------
static unsigned char* find_image_in_mem(const unsigned char* data, size_t len, int* w, int* h) {
    static const unsigned char png_sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (!data || len < 16) return NULL;
    int tried = 0;
    for (size_t i = 0; i + 16 <= len && tried < 12; i++) {
        int is_png = (i + 8 <= len && !memcmp(data + i, png_sig, 8));
        int is_jpg = (data[i] == 0xFF && data[i + 1] == 0xD8 && data[i + 2] == 0xFF);
        if (!is_png && !is_jpg) continue;
        tried++;
        int iw = 0, ih = 0;
        unsigned char* img = stbi_load_from_memory(data + i, (int)(len - i), &iw, &ih, NULL, 4);
        if (img && iw > 16 && ih > 16) {
            *w = iw; *h = ih;
            return img;
        }
        if (img) stbi_image_free(img);
    }
    return NULL;
}

#define ART_SCAN_LIMIT (2u * 1024u * 1024u)

static void fallback_art(const char* path, TrackMeta* m, size_t scan_limit) {
    if (m->art) return;
    FILE* f = plat_fopen(path, "rb");
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

static uint32_t meta_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int meta_mp4_find(FILE* f, long start, long end, const char tag[4], long* ds, long* de) {
    long pos = start;
    unsigned char hdr[16];
    while (pos + 8 <= end) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint32_t sz32 = meta_be32(hdr);
        long hlen = 8, sz = (long)sz32;
        if (sz32 == 1) {
            if (pos + 16 > end || fread(hdr + 8, 1, 8, f) != 8) break;
            uint64_t sz64 = ((uint64_t)meta_be32(hdr + 8) << 32) | meta_be32(hdr + 12);
            if (sz64 < 16 || sz64 > 0x7FFFFFFFULL) break;
            sz = (long)sz64; hlen = 16;
        } else if (sz32 == 0) {
            sz = end - pos;
        }
        if (sz < hlen || pos + sz > end) break;
        if (memcmp(hdr + 4, tag, 4) == 0) {
            *ds = pos + hlen; *de = pos + sz;
            return 1;
        }
        pos += sz;
    }
    return 0;
}

static void mp4_read_str_atom(FILE* f, long s, long e, char* out, size_t cap) {
    long ds, de;
    if (!meta_mp4_find(f, s, e, "data", &ds, &de) || ds + 8 >= de) return;
    long n = de - (ds + 8);
    if (n <= 0) return;
    if ((size_t)n >= cap) n = (long)cap - 1;
    if (fseek(f, ds + 8, SEEK_SET) != 0) return;
    size_t got = fread(out, 1, (size_t)n, f);
    out[got] = 0;
}

static int parse_mp4_meta(const char* path, TrackMeta* m) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    long s0, e0, s1, e1;
    if (fsz < 16 || !meta_mp4_find(f, 0, fsz, "moov", &s0, &e0) ||
        !meta_mp4_find(f, s0, e0, "udta", &s1, &e1) ||
        !meta_mp4_find(f, s1, e1, "meta", &s0, &e0) || s0 + 4 > e0 ||
        !meta_mp4_find(f, s0 + 4, e0, "ilst", &s1, &e1)) {
        fclose(f);
        return 0;
    }
    int got = 0;
    long pos = s1;
    unsigned char hdr[8];
    while (pos + 8 <= e1) {
        if (fseek(f, pos, SEEK_SET) != 0 || fread(hdr, 1, 8, f) != 8) break;
        uint32_t sz = meta_be32(hdr);
        if (sz < 8 || pos + (long)sz > e1) break;
        long as = pos + 8, ae = pos + (long)sz;
        if (hdr[0+4] == 0xA9 && memcmp(hdr + 5, "nam", 3) == 0) { mp4_read_str_atom(f, as, ae, m->title, sizeof(m->title)); got = 1; }
        else if (hdr[0+4] == 0xA9 && memcmp(hdr + 5, "ART", 3) == 0) { mp4_read_str_atom(f, as, ae, m->artist, sizeof(m->artist)); got = 1; }
        else if (hdr[0+4] == 0xA9 && memcmp(hdr + 5, "alb", 3) == 0) { mp4_read_str_atom(f, as, ae, m->album, sizeof(m->album)); got = 1; }
        else if (memcmp(hdr + 4, "covr", 4) == 0 && !m->art) {
            long ds, de;
            if (meta_mp4_find(f, as, ae, "data", &ds, &de) && ds + 8 < de) {
                long n = de - (ds + 8);
                if (n > 32 && n <= 8 * 1024 * 1024) {
                    unsigned char* buf = (unsigned char*)malloc((size_t)n);
                    if (buf && fseek(f, ds + 8, SEEK_SET) == 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) {
                        int w = 0, h = 0;
                        unsigned char* img = stbi_load_from_memory(buf, (int)n, &w, &h, NULL, 4);
                        if (img && w > 16 && h > 16) { m->art = img; m->art_w = w; m->art_h = h; got = 1; }
                        else if (img) stbi_image_free(img);
                    }
                    free(buf);
                }
            }
        }
        pos += (long)sz;
    }
    fclose(f);
    return got;
}

static int parse_aiff_meta(const char* path, TrackMeta* m) {
    FILE* f = plat_fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "FORM", 4) != 0) { fclose(f); return 0; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f), pos = 12;
    int got = 0;
    while (pos + 8 <= fsz) {
        unsigned char ck[8];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = meta_be32(ck + 4);
        long cdata = pos + 8;
        if (cdata + (long)sz > fsz) break;
        if (memcmp(ck, "NAME", 4) == 0 && sz > 0 && sz < 512 && !m->title[0]) {
            size_t n = sz < sizeof(m->title) - 1 ? sz : sizeof(m->title) - 1;
            if (fread(m->title, 1, n, f) == n) { m->title[n] = 0; got = 1; }
        } else if (memcmp(ck, "AUTH", 4) == 0 && sz > 0 && sz < 512 && !m->artist[0]) {
            size_t n = sz < sizeof(m->artist) - 1 ? sz : sizeof(m->artist) - 1;
            if (fread(m->artist, 1, n, f) == n) { m->artist[n] = 0; got = 1; }
        }
        pos = cdata + (long)((sz + 1u) & ~1u);
    }
    fclose(f);
    return got;
}

static int meta_load_impl(const char* path, TrackMeta* m, size_t scan_limit, int check_ext) {
    memset(m, 0, sizeof(*m));
    int got = 0;
    if (ci_ends_with(path, ".flac")) got = parse_flac(path, m);
    else if (ci_ends_with(path, ".mp3")) {
        Id3 id;
        if (id3_open(path, &id)) {
            got = id3_parse(&id, m);
            free(id.body);
        }
    } else if (ci_ends_with(path, ".ogg") || ci_ends_with(path, ".oga")) got = parse_ogg(path, m);
    else if (ci_ends_with(path, ".m4a") || ci_ends_with(path, ".mp4") || ci_ends_with(path, ".m4b") || ci_ends_with(path, ".alac"))
        got = parse_mp4_meta(path, m);
    else if (ci_ends_with(path, ".aiff") || ci_ends_with(path, ".aif") || ci_ends_with(path, ".aifc"))
        got = parse_aiff_meta(path, m);

    fallback_art(path, m, scan_limit);
    if (check_ext && !m->art) try_external_cover(path, m);
    if (m->art) got = 1;

    if (!m->title[0]) basename_no_ext(path, m->title, sizeof(m->title));
    return got;
}

int meta_load(const char* path, TrackMeta* m) {
    return meta_load_impl(path, m, ART_SCAN_LIMIT, 1);
}

int meta_load_quick(const char* path, TrackMeta* m) {
    return meta_load_impl(path, m, 384u * 1024u, 1);
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
