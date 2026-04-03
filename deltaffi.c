/*
 * deltaffi.c
 *
 * Fast C core for LuaJIT FFI on Termux.
 *
 * Exports:
 * - dl_fnv1a64
 * - dl_reverse_canonical
 * - dl_inter_reversed
 * - dl_xor_delta
 * - dl_block_hashes
 * - dl_pack_rle
 * - dl_unpack_rle
 *
 * Build on Termux:
 *   clang -O3 -fPIC -shared deltaffi.c -o libdeltaffi.so
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define DL_EXPORT __attribute__((visibility("default")))
#else
#define DL_EXPORT
#endif

DL_EXPORT uint64_t dl_fnv1a64(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

DL_EXPORT void dl_reverse_canonical(const uint8_t *src, size_t len, uint8_t *dst) {
    size_t i;
    if (!src || !dst) return;
    for (i = 0; i < len; ++i) {
        dst[i] = src[len - 1 - i];
    }
}

DL_EXPORT void dl_inter_reversed(const uint8_t *src, size_t len, uint8_t *dst) {
    size_t pair_count, i;
    if (!src || !dst) return;

    pair_count = len / 2;

    for (i = 0; i < pair_count; ++i) {
        size_t src_pair = (pair_count - 1 - i) * 2;
        size_t dst_pair = i * 2;
        dst[dst_pair] = src[src_pair];
        dst[dst_pair + 1] = src[src_pair + 1];
    }

    if (len & 1) {
        dst[len - 1] = src[len - 1];
    }
}

DL_EXPORT void dl_xor_delta(
    const uint8_t *a, size_t a_len,
    const uint8_t *b, size_t b_len,
    uint8_t *out
) {
    size_t i, n;
    if (!out) return;

    n = (a_len > b_len) ? a_len : b_len;
    for (i = 0; i < n; ++i) {
        uint8_t av = (i < a_len && a) ? a[i] : 0;
        uint8_t bv = (i < b_len && b) ? b[i] : 0;
        out[i] = (uint8_t)(av ^ bv);
    }
}

DL_EXPORT size_t dl_block_hashes(
    const uint8_t *data,
    size_t len,
    size_t block_size,
    uint64_t *out,
    size_t out_cap
) {
    size_t count = 0;
    size_t off;

    if (!out || out_cap == 0) return 0;
    if (block_size == 0) return 0;

    if (len == 0) {
        out[0] = dl_fnv1a64((const uint8_t *)"", 0);
        return 1;
    }

    for (off = 0; off < len; off += block_size) {
        size_t n = block_size;
        if (off + n > len) n = len - off;
        if (count >= out_cap) break;
        out[count++] = dl_fnv1a64(data + off, n);
    }

    return count;
}

/*
 * Compact RLE:
 * marker 0xFF, count, byte
 * repeated runs >= 4 are packed
 * literal 0xFF is always packed
 *
 * Returns required size if out == NULL or out_cap == 0.
 */
DL_EXPORT size_t dl_pack_rle(
    const uint8_t *src,
    size_t len,
    uint8_t *out,
    size_t out_cap
) {
    size_t i = 0;
    size_t written = 0;

    while (i < len) {
        uint8_t b = src[i];
        size_t run = 1;

        while ((i + run) < len && run < 255 && src[i + run] == b) {
            run++;
        }

        if (run >= 4 || b == 0xFF) {
            if (out && (written + 3) <= out_cap) {
                out[written] = 0xFF;
                out[written + 1] = (uint8_t)run;
                out[written + 2] = b;
            }
            written += 3;
            i += run;
        } else {
            size_t j;
            for (j = 0; j < run; ++j) {
                if (out && (written + 1) <= out_cap) {
                    out[written] = src[i + j];
                }
                written += 1;
            }
            i += run;
        }
    }

    return written;
}

/*
 * Returns output size, or 0 on corrupt stream if strict decode fails.
 */
DL_EXPORT size_t dl_unpack_rle(
    const uint8_t *src,
    size_t len,
    uint8_t *out,
    size_t out_cap
) {
    size_t i = 0;
    size_t written = 0;

    while (i < len) {
        uint8_t b = src[i];
        if (b == 0xFF) {
            size_t j;
            uint8_t count;
            uint8_t val;

            if ((i + 2) >= len) return 0;

            count = src[i + 1];
            val = src[i + 2];

            for (j = 0; j < count; ++j) {
                if (out && written < out_cap) {
                    out[written] = val;
                }
                written++;
            }

            i += 3;
        } else {
            if (out && written < out_cap) {
                out[written] = b;
            }
            written++;
            i++;
        }
    }

    return written;
}
