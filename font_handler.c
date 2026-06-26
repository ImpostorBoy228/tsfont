#include "font_handler.h"
#include <stdlib.h>
#include <string.h>
#include <ft2build.h>

#include FT_FREETYPE_H

static int is_cont(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

static uint32_t deutf8(const unsigned char **p, const unsigned char *end) {
    const unsigned char *s = *p;
    if (s >= end) return 0;

    uint32_t c;
    uint32_t min_cp;
    if ((s[0] & 0x80) == 0) {
        c = s[0];
        *p += 1;
        return c;
    } else if ((s[0] & 0xE0) == 0xC0) {
        if (s + 1 >= end || !is_cont(s[1])) { *p = end; return 0xFFFD; }
        c = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        min_cp = 0x80;
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        if (s + 2 >= end || !is_cont(s[1]) || !is_cont(s[2])) { *p = end; return 0xFFFD; }
        c = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        min_cp = 0x800;
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        if (s + 3 >= end || !is_cont(s[1]) || !is_cont(s[2]) || !is_cont(s[3])) { *p = end; return 0xFFFD; }
        c = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) | ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        min_cp = 0x10000;
        *p += 4;
    } else {
        *p += 1;
        return 0xFFFD;
    }

    // reject overlong, surrogates, > U+10FFFF
    if (c < min_cp || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) {
        return 0xFFFD;
    }
    return c;
}

void* font_load(const unsigned char *data, unsigned long len, float pixel_size) {
    FT_Library library;
    FT_Error error = FT_Init_FreeType(&library);
    if (error) return NULL;

    FT_Face face;
    error = FT_New_Memory_Face(library, data, (FT_Long)len, 0, &face);
    if (error) {
        FT_Done_FreeType(library);
        return NULL;
    }

    error = FT_Set_Pixel_Sizes(face, 0, (FT_UInt)(pixel_size + 0.5f));
    if (error) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return NULL;
    }

    struct FontHandle {
        FT_Face face;
        FT_Library library;
    };
    struct FontHandle *handle = malloc(sizeof(*handle));
    if (!handle) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return NULL;
    }
    handle->face = face;
    handle->library = library;
    return handle;
}

void font_free(void *font) {
    if (!font) return;
    struct FontHandle { FT_Face face; FT_Library library; };
    struct FontHandle *handle = (struct FontHandle*)font;
    FT_Done_Face(handle->face);
    FT_Done_FreeType(handle->library);
    free(handle);
}

float cock_measure(void *font, const char *utf8, unsigned long len) {
    /// text width measurement in px

    if (!font || !utf8 || len == 0) return 0.0f;
    struct FontHandle { FT_Face face; FT_Library library; };
    FT_Face face = ((struct FontHandle*)font)->face;

    const unsigned char *p = (const unsigned char*)utf8;
    const unsigned char *end = p + len;
    float pen_x = 0.0f;
    uint32_t prev_cp = 0;
    int first = 1;

    while (p < end) {
        uint32_t cp = deutf8(&p, end);
        FT_UInt glyph_index = FT_Get_Char_Index(face, cp);
        if (glyph_index == 0) continue; // buzz dont care ts
        if (!first) {
            FT_Vector kerning;
            FT_Get_Kerning(face, FT_Get_Char_Index(face, prev_cp), glyph_index,
                           FT_KERNING_DEFAULT, &kerning);
            pen_x += kerning.x / 64.0f;
        }

        FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
        pen_x += face->glyph->advance.x / 64.0f;

        first = 0;
        prev_cp = cp;
    }
    return pen_x; // so sigma
}

int font_fill_glyphs(void *font,
                     const char *utf8, unsigned long len,
                     GlyphInfo *out_infos, int max_glyphs,
                     unsigned char **out_bitmap, unsigned long *out_bitmap_size) {
    if (!font || !utf8 || len == 0 || !out_infos || max_glyphs <= 0)
        return -1;

    struct FontHandle { FT_Face face; FT_Library library; };
    FT_Face face = ((struct FontHandle*)font)->face;
    const unsigned char *p = (const unsigned char*)utf8;
    const unsigned char *end = p + len;

    int glyph_count = 0;
    size_t total_bytes = 0;
    const unsigned char *scan = p;
    int first = 1;

    while (scan < end && glyph_count < max_glyphs) {
        uint32_t cp = deutf8(&scan, end);
        FT_UInt glyph_index = FT_Get_Char_Index(face, cp);
        if (glyph_index == 0) continue;

        FT_Error error = FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER);
        if (error) return -1;

        FT_GlyphSlot slot = face->glyph;
        total_bytes += (size_t)slot->bitmap.rows * (size_t)slot->bitmap.pitch;
        glyph_count++;
        first = 0;
    }

    unsigned char *bitmap_buf = NULL;
    if (total_bytes > 0) {
        bitmap_buf = (unsigned char*)malloc(total_bytes);
        if (!bitmap_buf) return -1;
    }

    p = (const unsigned char*)utf8; // ptr drop
    int idx = 0;
    size_t byte_offset = 0;
    uint32_t prev_glyph_index = 0;
    first = 1;

    while (p < end && idx < max_glyphs) {
        uint32_t cp = deutf8(&p, end);
        FT_UInt glyph_index = FT_Get_Char_Index(face, cp);
        if (glyph_index == 0) continue;

        // повторный рендер
        FT_Error error = FT_Load_Glyph(face, glyph_index, FT_LOAD_RENDER);
        if (error) {
            free(bitmap_buf);
            return -1;
        }

        FT_GlyphSlot slot = face->glyph;
        GlyphInfo *info = &out_infos[idx];
        info->codepoint = cp;
        info->adv_x = slot->advance.x / 64.0f;
        info->br_x   = (float)slot->bitmap_left;
        info->br_y   = (float)slot->bitmap_top;
        info->bm_width  = slot->bitmap.width;
        info->bm_rows   = slot->bitmap.rows;
        info->bm_pitch  = slot->bitmap.pitch;
        info->bm_offset = (int)byte_offset;

        // Кернинг с предыдущим глифом
        if (!first) {
            FT_Vector kerning;
            FT_Get_Kerning(face, prev_glyph_index, glyph_index,
                           FT_KERNING_DEFAULT, &kerning);
            info->ker_x = kerning.x / 64.0f;
            info->ker_y = kerning.y / 64.0f;
        } else {
            info->ker_x = 0.0f;
            info->ker_y = 0.0f;
        }

        // Копируем bitmap в общий буфер
        size_t bm_bytes = (size_t)slot->bitmap.rows * (size_t)slot->bitmap.pitch;
        if (bm_bytes > 0 && bitmap_buf) {
            memcpy(bitmap_buf + byte_offset, slot->bitmap.buffer, bm_bytes);
        }
        byte_offset += bm_bytes;

        prev_glyph_index = glyph_index;
        first = 0;
        idx++;
    }

    *out_bitmap = bitmap_buf;
    *out_bitmap_size = total_bytes;
    return idx;
}

void free_bitmap_buffer(unsigned char *ptr) {
    free(ptr);
}
