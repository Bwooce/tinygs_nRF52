/*
 * BitCode — FSK frame decoders ported from ESP32 TinyGS
 * Original: Copyright (C) 2022-2023 @estbhan (GPL-3.0)
 *
 * Provides:
 *   - NRZ to NRZI conversion
 *   - AX.25 HDLC frame extraction (flag detection, bit unstuffing)
 *   - PN9 descrambler (fixed 8-bit shift)
 *   - x^17+x^12 descrambler
 */

#ifndef BITCODE_H
#define BITCODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode NRZ-S encoded FSK data into AX.25 frame.
 * Prepends sync word, converts NRZ→NRZI, removes bit stuffing,
 * inverts bit order per byte.
 *
 * framing=1: NRZS → AX.25
 * framing=3: Scrambled(x17x12) → NRZS → AX.25
 *
 * Returns 0 on success, 1 on frame error.
 * Output in ax25bin, size in *out_len.
 */
int bitcode_nrz2ax25(const uint8_t *data, size_t data_len,
                     const uint8_t *sync_word, size_t sync_len,
                     uint8_t framing,
                     uint8_t *ax25bin, size_t *out_len, size_t out_max);

/*
 * PN9 descrambler (fixed 8-bit shift).
 * framing=2.
 * Output same size as input.
 */
void bitcode_pn9(const uint8_t *in, size_t len, uint8_t *out);

/*
 * Reverse bits in a single byte.
 */
uint8_t bitcode_reverse_byte(uint8_t b);

#ifdef __cplusplus
}
#endif

#endif /* BITCODE_H */
