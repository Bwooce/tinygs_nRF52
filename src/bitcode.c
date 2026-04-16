/*
 * BitCode — FSK frame decoders ported from ESP32 TinyGS
 * Original: Copyright (C) 2022-2023 @estbhan (GPL-3.0)
 * Ported to C for Zephyr by TinyGS nRF52 project.
 */

#include "bitcode.h"
#include <string.h>
#include <stdbool.h>

/* Read bit at position (1=LSB, 8=MSB) from a byte */
static inline uint8_t read_bit(uint8_t byte, int pos)
{
    return (byte >> (pos - 1)) & 1;
}

/* Write bit at position (1=LSB, 8=MSB) in a byte */
static inline void write_bit_byte(uint8_t *byte, int pos, int val)
{
    if (val) {
        *byte |= (1 << (pos - 1));
    } else {
        *byte &= ~(1 << (pos - 1));
    }
}

/* Read bit at position from uint32_t */
static inline int read_bit32(uint32_t val, int pos)
{
    return (val >> (pos - 1)) & 1;
}

static inline void write_bit32(uint32_t *val, int pos, int bit)
{
    if (bit) {
        *val |= (1u << (pos - 1));
    } else {
        *val &= ~(1u << (pos - 1));
    }
}

uint8_t bitcode_reverse_byte(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 1; i <= 8; i++) {
        r = (r << 1) | read_bit(b, i);
    }
    return r;
}

/* NRZ to NRZI conversion */
static void nrz2nrzi(const uint8_t *in, size_t len, uint8_t *out, size_t *out_len)
{
    uint8_t prev = 0;
    size_t idx = 0;

    for (size_t p = 0; p < len; p++) {
        uint8_t byte_in = in[p];
        uint8_t byte_out = 0;
        for (int i = 8; i > 0; i--) {
            uint8_t cur = read_bit(byte_in, i);
            if (prev == cur) {
                byte_out |= (1 << (i - 1));
            }
            prev = cur;
        }
        out[idx++] = byte_out;
    }
    *out_len = idx;
}

/* x^17+x^12 descrambler */
static void descram1712(const uint8_t *in, size_t len, uint8_t *out)
{
    uint32_t lfsr = 0;

    for (size_t idx = 0; idx < len; idx++) {
        uint8_t byte_in = in[idx];
        uint8_t byte_out = 0;

        for (int bit_pos = 8; bit_pos > 0; bit_pos--) {
            int x1 = read_bit32(lfsr, 12) ^ read_bit32(lfsr, 17);
            int bit = read_bit(byte_in, bit_pos);
            write_bit_byte(&byte_out, bit_pos, bit ^ x1);
            lfsr = lfsr << 1;
            write_bit32(&lfsr, 1, bit);
        }

        out[idx] = byte_out;
    }
}

/* Remove AX.25 bit stuffing and extract frame */
static int remove_bit_stuffing(const uint8_t *in, size_t in_len,
                               uint8_t *out, size_t *out_len, size_t out_max)
{
    int ones = 0, k = 8;
    uint8_t byte_in, byte_out = 0;
    int bit1 = 0, bit2, bit3;
    int resync = 0;
    bool error = true;
    bool flag_found = false;
    bool skip = false;
    bool store = false;
    *out_len = 0;

    byte_in = in[0];
    bit2 = read_bit(byte_in, 8);
    bit3 = read_bit(byte_in, 7);
    int j = 6;
    size_t i = 0;

    while (i < in_len) {
        byte_in = in[i];
        while (j > 0) {
            bit1 = bit2;
            bit2 = bit3;
            bit3 = read_bit(byte_in, j);

            if (resync > 0) {
                resync--;
                skip = true;
            }

            if (!skip && !flag_found) {
                if (bit1 == 0) {
                    if (store) write_bit_byte(&byte_out, k, 0);
                    k--;
                    ones = 0;
                    if (k == 0) {
                        if (store && *out_len < out_max) {
                            out[(*out_len)++] = byte_out;
                        }
                        k = 8;
                        byte_out = 0;
                    }
                } else {
                    ones++;
                    if (store) write_bit_byte(&byte_out, k, 1);
                    k--;
                    if (k == 0) {
                        if (store && *out_len < out_max) {
                            out[(*out_len)++] = byte_out;
                        }
                        k = 8;
                        byte_out = 0;
                    }
                    if (ones == 5) {
                        if (bit2 == 1) {
                            j = 0;
                            if (bit3 == 0) {
                                flag_found = true;
                                if (!store) store = true;
                                if (*out_len < 16 || k != 2) {
                                    *out_len = 0;
                                    k = 8;
                                    error = true;
                                    flag_found = false;
                                    resync = 2;
                                } else {
                                    if (k == 2) {
                                        error = false;
                                        i = in_len; /* break outer */
                                    } else {
                                        error = true;
                                    }
                                }
                            } else {
                                error = true;
                            }
                        } else {
                            ones = 0;
                            skip = true;
                        }
                    }
                }
            } else {
                skip = false;
            }
            j--;
        }
        j = 8;
        i++;
    }

    return (flag_found && !error) ? 0 : 1;
}

int bitcode_nrz2ax25(const uint8_t *data, size_t data_len,
                     const uint8_t *sync_word, size_t sync_len,
                     uint8_t framing,
                     uint8_t *ax25bin, size_t *out_len, size_t out_max)
{
    if (data_len + sync_len < 16) {
        *out_len = 0;
        return 1;
    }

    /* Prepend sync word to packet data */
    size_t total = sync_len + data_len;
    /* Use stack buffer — FSK packets are small (<256 bytes typically) */
    uint8_t combined[512];
    if (total > sizeof(combined)) {
        *out_len = 0;
        return 1;
    }
    memcpy(combined, sync_word, sync_len);
    memcpy(combined + sync_len, data, data_len);

    uint8_t hdlc[512];
    size_t hdlc_len = 0;

    if (framing == 1) {
        /* NRZS → AX.25 */
        nrz2nrzi(combined, total, hdlc, &hdlc_len);
    } else if (framing == 3) {
        /* Scrambled(x17x12) → NRZS → AX.25 */
        uint8_t descrambled[512];
        descram1712(combined, total, descrambled);
        nrz2nrzi(descrambled, total, hdlc, &hdlc_len);
    } else {
        *out_len = 0;
        return 1;
    }

    /* Remove bit stuffing and extract AX.25 frame */
    uint8_t inverted[512];
    size_t inv_len = 0;
    int result = remove_bit_stuffing(hdlc, hdlc_len, inverted, &inv_len, sizeof(inverted));

    /* Invert bit order of each byte (AX.25 convention) */
    *out_len = (inv_len > out_max) ? out_max : inv_len;
    for (size_t i = 0; i < *out_len; i++) {
        ax25bin[i] = bitcode_reverse_byte(inverted[i]);
    }

    return result;
}

void bitcode_pn9(const uint8_t *in, size_t len, uint8_t *out)
{
    uint32_t pn9 = 0x1FF;
    uint32_t mask = 0x1FF;

    for (size_t b = 0; b < len; b++) {
        out[b] = in[b] ^ (uint8_t)pn9;
        for (int k = 0; k < 8; k++) {
            write_bit32(&pn9, 10, read_bit32(pn9, 1) ^ read_bit32(pn9, 6));
            pn9 >>= 1;
            pn9 &= mask;
        }
    }
}
