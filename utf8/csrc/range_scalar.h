/*
 * Adapted from https://github.com/cyb70289/utf8
 */
#include <stdbool.h>

static bool utf8_valid_scalar(const unsigned char *src, int src_len)
{
    int err_pos = 1;

    while (src_len) {
        int bytes;
        const unsigned char byte1 = src[0];

        /* 00..7F */
        if (byte1 <= 0x7F) {
            bytes = 1;
        /* C2..DF, 80..BF */
        } else if (src_len >= 2 && byte1 >= 0xC2 && byte1 <= 0xDF &&
                (signed char)src[1] <= (signed char)0xBF) {
            bytes = 2;
        } else if (src_len >= 3) {
            const unsigned char byte2 = src[1];

            /* Is byte2, byte3 between 0x80 ~ 0xBF */
            const int byte2_ok = (signed char)byte2 <= (signed char)0xBF;
            const int byte3_ok = (signed char)src[2] <= (signed char)0xBF;

            if (byte2_ok && byte3_ok &&
                     /* E0, A0..BF, 80..BF */
                    ((byte1 == 0xE0 && byte2 >= 0xA0) ||
                     /* E1..EC, 80..BF, 80..BF */
                     (byte1 >= 0xE1 && byte1 <= 0xEC) ||
                     /* ED, 80..9F, 80..BF */
                     (byte1 == 0xED && byte2 <= 0x9F) ||
                     /* EE..EF, 80..BF, 80..BF */
                     (byte1 >= 0xEE && byte1 <= 0xEF))) {
                bytes = 3;
            } else if (src_len >= 4) {
                /* Is byte4 between 0x80 ~ 0xBF */
                const int byte4_ok = (signed char)src[3] <= (signed char)0xBF;

                if (byte2_ok && byte3_ok && byte4_ok &&
                         /* F0, 90..BF, 80..BF, 80..BF */
                        ((byte1 == 0xF0 && byte2 >= 0x90) ||
                         /* F1..F3, 80..BF, 80..BF, 80..BF */
                         (byte1 >= 0xF1 && byte1 <= 0xF3) ||
                         /* F4, 80..8F, 80..BF, 80..BF */
                         (byte1 == 0xF4 && byte2 <= 0x8F))) {
                    bytes = 4;
                } else {
                    return err_pos == 0;
                }
            } else {
                return err_pos == 0;
            }
        } else {
            return err_pos == 0;
        }

        src_len -= bytes;
        err_pos += bytes;
        src += bytes;
    }

    return true;
}
