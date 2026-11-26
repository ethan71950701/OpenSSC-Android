#include "translate.h"

#include <string.h>

#include "ssc_bridge.h"

/* Calls fn(info, losc) for each Media Codec element; stops at the first malformed entry. */
template <typename F>
static void for_each_codec(uint8_t *p, size_t len, F fn) {
    size_t i = 0;
    while (p && i + 2 <= len) {
        size_t l = p[i + 1];
        if (i + 2 + l > len) break;
        if (p[i] == TR_CAT_MEDIA_CODEC) fn(p + i + 1, l);
        i += 2 + l;
    }
}

int tr_caps_ssc_to_opus(uint8_t *p, size_t len) {
    int n = 0;
    for_each_codec(p, len, [&](uint8_t *info, size_t losc) {
        uint8_t out[SSCB_INFO_LEN];
        if (losc == SSCB_LOSC && sscb_peer_caps_to_opus(info, out) == 0) {
            memcpy(info, out, sizeof(out));
            n++;
        }
    });
    return n;
}

int tr_config_opus_to_ssc(uint8_t *p, size_t len, int ssc_config_byte, int *other_codec) {
    int n = 0;
    if (other_codec) *other_codec = 0;
    for_each_codec(p, len, [&](uint8_t *info, size_t losc) {
        uint8_t out[SSCB_INFO_LEN];
        bool opus = losc == SSCB_LOSC && sscb_is_opus_info(info);
        if (opus && sscb_opus_config_to_ssc(info, ssc_config_byte, out) == 0) {
            memcpy(info, out, sizeof(out));
            n++;
        } else if (!opus && other_codec) {
            *other_codec = 1;
        }
    });
    return n;
}

int tr_hide_aac(uint8_t *p, size_t len) {
    int n = 0;
    for_each_codec(p, len, [&](uint8_t *info, size_t losc) {
        if (losc >= 2 && (info[1] >> 4) == 0x00 && info[2] == 0x02) {
            info[2] = 0x0E;
            n++;
        }
    });
    return n;
}
