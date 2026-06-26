#include <stdint.h>

typedef struct {
    uint32_t codepoint;
    float    adv_x;
    float    br_x;
    float    br_y;
    int      bm_width;
    int      bm_rows;
    int      bm_pitch;
    int      bm_offset;
    float    ker_x;
    float    ker_y;
} GlyphInfo;
