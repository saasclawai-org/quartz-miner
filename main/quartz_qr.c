/**
 * quartz_qr.c — QR Code Generator for ESP32 Display
 *
 * Implements QR code encoding (versions 1-10, all ECC levels)
 * and renders directly to the ILI9341 display via quartz_display.
 *
 * QR encoding steps:
 * 1. Data analysis → pick version + mode
 * 2. Data encoding (byte mode)
 * 3. Error correction (Reed-Solomon)
 * 4. Module placement (finder patterns, timing, data, format)
 * 5. Render to display
 *
 * For payment URLs (~60 chars), version 3-5 with ECC-M is sufficient.
 */

#include "quartz_qr.h"
#include "qrcodegen.h"
#include "quartz_display.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "QZ.QR";
#endif

/* === QR Code Capacity Tables (byte mode, characters) === */
/* Version:  1   2   3   4   5   6   7   8   9  10 */
static const int qr_byte_capacity[4][10] = {
    /* ECC-L */ { 17, 32, 53, 78, 106, 134, 154, 192, 230, 271 },
    /* ECC-M */ { 14, 26, 42, 62,  84, 106, 122, 152, 180, 213 },
    /* ECC-Q */ { 11, 20, 32, 46,  60,  74,  86, 108, 130, 151 },
    /* ECC-H */ {  7, 14, 24, 34,  44,  58,  64,  84,  98, 119 },
};

/* Modules per side for each version (version N = 4N+17) */
int qr_modules(int version) {
    return 4 * version + 17;
}

/* Total codewords per version */
static const int qr_total_codewords[10] = {
    26, 44, 70, 100, 134, 172, 196, 242, 292, 346
};

/* ECC codewords per block */
static const int qr_ec_codewords[4][10] = {
    /* ECC-L */ { 7, 10, 15, 20, 26, 18, 20, 24, 30, 18 },
    /* ECC-M */ { 10, 16, 26, 18, 18, 24, 18, 22, 22, 26 },
    /* ECC-Q */ { 13, 22, 18, 26, 22, 16, 24, 22, 20, 24 },
    /* ECC-H */ { 17, 28, 22, 16, 22, 28, 26, 26, 24, 28 },
};

/* Number of blocks in group 1 / group 2 and data codewords per block */
/* Simplified: total blocks per version+ecc */
static const int qr_blocks[4][10] = {
    /* ECC-L */ { 1, 1, 1, 1, 1, 2, 2, 2, 2, 4 },
    /* ECC-M */ { 1, 1, 1, 2, 2, 4, 4, 4, 5, 5 },
    /* ECC-Q */ { 1, 1, 2, 2, 4, 4, 6, 6, 8, 8 },
    /* ECC-H */ { 1, 1, 2, 4, 4, 4, 5, 6, 8, 8 },
};

/* Block structure: group 1 (count, data_cw_per_block) + group 2 (count, data_cw_per_block) */
/* From ISO/IEC 18004 Table 9 — exact for versions 1-10, all ECC levels */
typedef struct {
    int g1_blocks;  /* number of blocks in group 1 */
    int g1_data;    /* data codewords per block in group 1 */
    int g2_blocks;  /* number of blocks in group 2 (0 for single-group) */
    int g2_data;    /* data codewords per block in group 2 */
} qr_block_layout_t;

static const qr_block_layout_t qr_block_layout[4][10] = {
    /* ECC-L */ {
        {1, 19, 0, 0},  /* v1 */
        {1, 34, 0, 0},  /* v2 */
        {1, 55, 0, 0},  /* v3 */
        {1, 80, 0, 0},  /* v4 */
        {1, 108, 0, 0}, /* v5 */
        {2, 68, 0, 0},  /* v6 */
        {2, 78, 0, 0},  /* v7 */
        {2, 97, 0, 0},  /* v8 */
        {2, 116, 0, 0}, /* v9 */
        {2, 68, 2, 69}, /* v10 */
    },
    /* ECC-M */ {
        {1, 16, 0, 0},  /* v1 */
        {1, 28, 0, 0},  /* v2 */
        {1, 44, 0, 0},  /* v3 */
        {2, 32, 0, 0},  /* v4 */
        {2, 49, 0, 0},  /* v5 */
        {4, 19, 0, 0},  /* v6 */
        {4, 31, 0, 0},  /* v7 */
        {2, 38, 2, 39}, /* v8 */
        {3, 36, 2, 37}, /* v9 */
        {4, 43, 1, 44}, /* v10 */
    },
    /* ECC-Q */ {
        {1, 13, 0, 0},  /* v1 */
        {1, 22, 0, 0},  /* v2 */
        {2, 17, 0, 0},  /* v3 */
        {2, 24, 0, 0},  /* v4 */
        {2, 11, 2, 12}, /* v5 */
        {4, 27, 0, 0},  /* v6 */
        {2, 8, 4, 9},   /* v7 */
        {4, 18, 2, 19}, /* v8 */
        {4, 16, 4, 17}, /* v9 */
        {6, 19, 2, 20}, /* v10 */
    },
    /* ECC-H */ {
        {1, 9, 0, 0},   /* v1 */
        {1, 16, 0, 0},  /* v2 */
        {2, 13, 0, 0},  /* v3 */
        {4, 9, 0, 0},   /* v4 */
        {2, 11, 2, 12}, /* v5 */
        {4, 15, 0, 0},  /* v6 */
        {4, 13, 1, 14}, /* v7 */
        {4, 14, 2, 15}, /* v8 */
        {4, 12, 4, 13}, /* v9 */
        {6, 15, 2, 16}, /* v10 */
    },
};

/* === Galois Field arithmetic for Reed-Solomon === */
/* GF(256) with primitive polynomial 0x11D */

static uint8_t gf_exp[512];
static uint8_t gf_log[256];

static void gf_init(void) {
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

/* Generate generator polynomial for n ECC codewords */
static void rs_generator(int n, uint8_t *gen) {
    gen[0] = 1;
    int len = 1;
    for (int i = 0; i < n; i++) {
        /* Multiply by (x - α^i) */
        for (int j = len; j > 0; j--) {
            gen[j] = gen[j - 1] ^ gf_mul(gen[j], gf_exp[i]);
        }
        gen[0] = gf_mul(gen[0], gf_exp[i]);
        len++;
    }
}

/* Compute ECC codewords using polynomial division */
static void rs_encode(const uint8_t *data, int data_len,
                      const uint8_t *gen, int ec_len,
                      uint8_t *ec_out)
{
    memset(ec_out, 0, ec_len);
    for (int i = 0; i < data_len; i++) {
        uint8_t factor = data[i] ^ ec_out[0];
        memmove(ec_out, ec_out + 1, ec_len - 1);
        ec_out[ec_len - 1] = 0;
        if (factor != 0) {
            for (int j = 0; j < ec_len; j++) {
                ec_out[j] ^= gf_mul(gen[j + 1], factor);
            }
        }
    }
}

/* === QR Module Matrix === */

/* Get module value from matrix (handles masking lookup) */
typedef struct {
    int version;
    int size;
    uint8_t modules[QR_MAX_MODULES * QR_MAX_MODULES]; /* 1 = dark, 0 = light */
    uint8_t reserved[QR_MAX_MODULES * QR_MAX_MODULES]; /* 1 = function pattern */
} qr_matrix_t;

static void qr_matrix_set(qr_matrix_t *m, int r, int c, uint8_t val) {
    if (r < 0 || r >= m->size || c < 0 || c >= m->size) return;
    m->modules[r * m->size + c] = val;
}

static uint8_t qr_matrix_get(qr_matrix_t *m, int r, int c) {
    if (r < 0 || r >= m->size || c < 0 || c >= m->size) return 0;
    return m->modules[r * m->size + c];
}

static void qr_matrix_reserve(qr_matrix_t *m, int r, int c) {
    m->reserved[r * m->size + c] = 1;
}

static int qr_matrix_is_reserved(qr_matrix_t *m, int r, int c) {
    return m->reserved[r * m->size + c];
}

/* Place finder pattern (7x7) at corner */
static void place_finder(qr_matrix_t *m, int r0, int c0) {
    for (int dr = 0; dr < 7; dr++) {
        for (int dc = 0; dc < 7; dc++) {
            int r = r0 + dr, c = c0 + dc;
            qr_matrix_reserve(m, r, c);

            /* Finder pattern: border ring + center 3x3 */
            int is_dark = 0;
            if (dr == 0 || dr == 6 || dc == 0 || dc == 6) is_dark = 1;
            else if (dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4) is_dark = 1;
            qr_matrix_set(m, r, c, is_dark);
        }
    }
}

/* Place timing patterns (row 6 and column 6) */
static void place_timing(qr_matrix_t *m) {
    for (int i = 8; i < m->size - 8; i++) {
        qr_matrix_reserve(m, 6, i);
        qr_matrix_set(m, 6, i, (i % 2 == 0) ? 1 : 0);
        qr_matrix_reserve(m, i, 6);
        qr_matrix_set(m, i, 6, (i % 2 == 0) ? 1 : 0);
    }
}

/* Reserve format info areas */
static void reserve_format(qr_matrix_t *m) {
    /* Around top-left finder */
    for (int i = 0; i <= 8; i++) {
        if (!qr_matrix_is_reserved(m, 8, i)) qr_matrix_reserve(m, 8, i);
        if (!qr_matrix_is_reserved(m, i, 8)) qr_matrix_reserve(m, i, 8);
    }
    /* Top-right */
    qr_matrix_reserve(m, 8, m->size - 8);
    for (int i = 0; i < 8; i++) qr_matrix_reserve(m, m->size - 1 - i, 8);
    /* Bottom-left */
    for (int i = 0; i < 7; i++) qr_matrix_reserve(m, 8, m->size - 7 + i);
}

/* Place alignment patterns (versions 2+) */
/* Alignment pattern center positions per version */
static const int align_pos[10][7] = {
    {},                          /* v1: none */
    {6, 18},                     /* v2 */
    {6, 22},                     /* v3 */
    {6, 26},                     /* v4 */
    {6, 30},                     /* v5 */
    {6, 34},                     /* v6 */
    {6, 22, 38},                 /* v7 */
    {6, 24, 42},                 /* v8 */
    {6, 26, 46},                 /* v9 */
    {6, 28, 50},                 /* v10 */
};

static void place_alignment(qr_matrix_t *m) {
    if (m->version < 2) return;
    const int *pos = align_pos[m->version - 1];
    int n = 0;
    while (pos[n] != 0 || n == 0) {
        if (pos[n] == 0 && n > 0) break;
        n++;
        if (n >= 7) break;
    }
    /* Actually count positions properly */
    n = 0;
    int positions[7];
    for (int i = 0; i < 7; i++) {
        if (align_pos[m->version - 1][i] == 0 && i > 0) break;
        positions[i] = align_pos[m->version - 1][i];
        n++;
    }

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int r = positions[i];
            int c = positions[j];
            /* Skip if overlapping finder patterns */
            if ((r <= 8 && c <= 8) || (r <= 8 && c >= m->size - 9) ||
                (r >= m->size - 9 && c <= 8)) continue;

            for (int dr = -2; dr <= 2; dr++) {
                for (int dc = -2; dc <= 2; dc++) {
                    int rr = r + dr, cc = c + dc;
                    qr_matrix_reserve(m, rr, cc);
                    int is_dark = (abs(dr) == 2 || abs(dc) == 2 || (dr == 0 && dc == 0)) ? 1 : 0;
                    qr_matrix_set(m, rr, cc, is_dark);
                }
            }
        }
    }
}

/* Dark module (always at row size-8, col 8) */
static void place_dark_module(qr_matrix_t *m) {
    qr_matrix_reserve(m, m->size - 8, 8);
    qr_matrix_set(m, m->size - 8, 8, 1);
}

/* Place data bits in zigzag pattern */
static void place_data(qr_matrix_t *m, const uint8_t *bits, int bit_count) {
    int col = m->size - 1;
    int direction = -1; /* up */
    int bit_idx = 0;

    while (col > 0 && bit_idx < bit_count) {
        if (col == 6) col--; /* skip timing column */

        for (int step = 0; step < m->size && bit_idx < bit_count; step++) {
            int r = (direction == -1) ? (m->size - 1 - step) : step;

            /* Try right then left column pair */
            for (int c_off = 0; c_off < 2 && bit_idx < bit_count; c_off++) {
                int c = col - c_off;
                if (c < 0) break;
                if (!qr_matrix_is_reserved(m, r, c)) {
                    qr_matrix_set(m, r, c, bits[bit_idx >> 3] & (0x80 >> (bit_idx & 7)) ? 1 : 0);
                    bit_idx++;
                }
            }
        }

        col -= 2;
        direction = -direction;
    }
}

/* Apply mask pattern 0: (r+c) % 2 == 0 */
static void apply_mask(qr_matrix_t *m) {
    for (int r = 0; r < m->size; r++) {
        for (int c = 0; c < m->size; c++) {
            if (qr_matrix_is_reserved(m, r, c)) continue;
            if ((r + c) % 2 == 0) {
                /* Toggle */
                m->modules[r * m->size + c] ^= 1;
            }
        }
    }
}

/* Place format info bits */
static void place_format(qr_matrix_t *m, qr_ecc_t ecc) {
    /* Format info for mask 0 */
    /* ECC indicator: L=01, M=00, Q=11, H=10 */
    static const int ecc_bits[4] = { 1, 0, 3, 2 };
    int format = (ecc_bits[ecc] << 3) | 0; /* mask 0 */
    /* BCH(15,5) encoding */
    int bch = format << 10;
    int gen = 0x537; /* x^10 + x^8 + x^5 + x^4 + x^2 + x + 1 */
    for (int i = 14; i >= 10; i--) {
        if (bch & (1 << i)) {
            bch ^= gen << (i - 10);
        }
    }
    int format_bits = ((format << 10) | (bch & 0x3FF)) ^ 0x5412; /* XOR with mask pattern */

    /* Place format bits */
    for (int i = 0; i < 15; i++) {
        int bit = (format_bits >> i) & 1;

        /* Top-left around finder */
        if (i < 6) {
            qr_matrix_set(m, 8, i, bit);
        } else if (i == 6) {
            qr_matrix_set(m, 8, 7, bit);
        } else if (i == 7) {
            qr_matrix_set(m, 8, 8, bit);
        } else if (i == 8) {
            qr_matrix_set(m, 7, 8, bit);
        } else {
            qr_matrix_set(m, 14 - i, 8, bit);
        }

        /* Top-right + bottom-left */
        if (i < 8) {
            qr_matrix_set(m, m->size - 1 - i, 8, bit);
        } else {
            qr_matrix_set(m, 8, m->size - 15 + i, bit);
        }
    }
}

/* === Public API === */

int quartz_qr_version_for_data(int data_len, qr_ecc_t ecc) {
    for (int v = 1; v <= 10; v++) {
        if (data_len <= qr_byte_capacity[ecc][v - 1]) return v;
    }
    return -1; /* too large */
}

int quartz_qr_display(
    const char *data,
    qr_ecc_t ecc,
    int x, int y,
    int scale,
    uint16_t fg_color,
    uint16_t bg_color
) {
    if (!data || scale < 1) return -1;

    /* Use Nayuki QR library for correct generation */
    enum qrcodegen_Ecc nayuki_ecc;
    switch (ecc) {
        case QR_ECC_LOW: nayuki_ecc = qrcodegen_Ecc_LOW; break;
        case QR_ECC_MEDIUM: nayuki_ecc = qrcodegen_Ecc_MEDIUM; break;
        case QR_ECC_QUARTILE: nayuki_ecc = qrcodegen_Ecc_QUARTILE; break;
        case QR_ECC_HIGH: nayuki_ecc = qrcodegen_Ecc_HIGH; break;
        default: nayuki_ecc = qrcodegen_Ecc_HIGH; break;
    }

    uint8_t *qrcode = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
    uint8_t *tempBuf = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
    if (!qrcode || !tempBuf) {
        free(qrcode); free(tempBuf);
        return -1;
    }

    bool ok = qrcodegen_encodeText(data, tempBuf, qrcode,
        nayuki_ecc, qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);

    if (!ok) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "QR encoding failed for data len=%zu", strlen(data));
#endif
        free(qrcode); free(tempBuf);
        return -1;
    }

    int size = qrcodegen_getSize(qrcode);
    int total_px = size * scale;

#ifdef ESP_PLATFORM
    ESP_LOGD(TAG, "QR %dx%d modules, scale=%d, at (%d,%d)", size, size, scale, x, y);
#endif

    /* Clear background area with proper 4-module quiet zone */
    int quiet = scale * 4;
    quartz_display_fill_rect(x - quiet, y - quiet,
                             total_px + quiet * 2, total_px + quiet * 2, bg_color);

    /* Draw modules as a single row-batched operation for SPI efficiency */
    /* Instead of one fill_rect per module (2809 SPI transactions),
     * build each row in a buffer and send horizontal runs in one shot */
    for (int r = 0; r < size; r++) {
        int c = 0;
        while (c < size) {
            /* Find next dark module (run of dark modules) */
            if (qrcodegen_getModule(qrcode, c, r)) {
                int run_start = c;
                while (c < size && qrcodegen_getModule(qrcode, c, r)) {
                    c++;
                }
                int run_len = c - run_start;
                quartz_display_fill_rect(
                    x + run_start * scale,
                    y + r * scale,
                    run_len * scale,
                    scale,
                    fg_color);
            } else {
                c++;
            }
        }
    }

#ifdef ESP_PLATFORM
    ESP_LOGD(TAG, "QR rendered at (%d,%d), %dx%d px", x, y, total_px, total_px);
#endif

    free(qrcode);
    free(tempBuf);
    return 0;
}

/* === Serial ASCII QR Output === */

int quartz_qr_serial(const char *data, qr_ecc_t ecc) {
    if (!data) return -1;

    /* Use Nayuki QR library */
    enum qrcodegen_Ecc nayuki_ecc;
    switch (ecc) {
        case QR_ECC_LOW: nayuki_ecc = qrcodegen_Ecc_LOW; break;
        case QR_ECC_MEDIUM: nayuki_ecc = qrcodegen_Ecc_MEDIUM; break;
        case QR_ECC_QUARTILE: nayuki_ecc = qrcodegen_Ecc_QUARTILE; break;
        case QR_ECC_HIGH: nayuki_ecc = qrcodegen_Ecc_HIGH; break;
        default: nayuki_ecc = qrcodegen_Ecc_HIGH; break;
    }

    uint8_t *qrcode = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
    uint8_t *tempBuf = malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(10));
    if (!qrcode || !tempBuf) {
        free(qrcode); free(tempBuf);
        return -1;
    }

    bool ok = qrcodegen_encodeText(data, tempBuf, qrcode,
        nayuki_ecc, qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);

    if (!ok) {
        free(qrcode); free(tempBuf);
        return -1;
    }

    int size = qrcodegen_getSize(qrcode);

    /* Output QR as ASCII art */
    printf("\r\n");
    /* Top quiet zone (4 modules) */
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < size + 8; c++) printf("  ");
        printf("\r\n");
    }
    for (int r = 0; r < size; r++) {
        /* Left quiet zone */
        for (int i = 0; i < 4; i++) printf("  ");
        for (int c = 0; c < size; c++) {
            /* Standard: dark module = ##, light = space */
            if (qrcodegen_getModule(qrcode, c, r)) printf("##");
            else printf("  ");
        }
        /* Right quiet zone */
        for (int i = 0; i < 4; i++) printf("  ");
        printf("\r\n");
    }
    /* Bottom quiet zone */
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < size + 8; c++) printf("  ");
        printf("\r\n");
    }
    printf("\r\n");
    fflush(stdout);

    free(qrcode);
    free(tempBuf);
    return 0;
}
