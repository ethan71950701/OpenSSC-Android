/* Unit tests for ssc_bridge, plus an end-to-end check with the open-source SSC encoder.
 * usage: test_bridge [path/to/tests/vectors]   (the end-to-end part is skipped without it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssc_bridge.h"
#include "ssc_encoder.h"

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* The SSC capability the Galaxy Buds 3 Pro reported to a Xiaomi 13 (from btsnoop). */
static const uint8_t kBudsSscCaps[SSCB_INFO_LEN] = {0x09, 0x00, 0xff, 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x3e};

static void test_ids(void) {
    uint32_t v = 0;
    uint16_t c = 0;
    CHECK(sscb_info_get_ids(kBudsSscCaps, &v, &c) == 0);
    CHECK(v == 0x75 && c == 0x0103);
    CHECK(sscb_is_ssc_info(kBudsSscCaps) == 1);
    CHECK(sscb_is_opus_info(kBudsSscCaps) == 0);

    uint8_t bad[SSCB_INFO_LEN];
    memcpy(bad, kBudsSscCaps, sizeof bad);
    bad[0] = 8; /* wrong LOSC */
    CHECK(sscb_info_get_ids(bad, &v, &c) == SSCB_ERR_ARG);
    memcpy(bad, kBudsSscCaps, sizeof bad);
    bad[2] = 0x02; /* AAC, not vendor */
    CHECK(sscb_is_ssc_info(bad) == 0);
    CHECK(sscb_info_get_ids(NULL, &v, &c) == SSCB_ERR_ARG);
}

static void test_caps_to_opus(void) {
    static const uint8_t want[SSCB_INFO_LEN] = {0x09, 0x00, 0xff, 0xe0, 0x00, 0x00, 0x00, 0x01, 0x00, 0x92};
    uint8_t out[SSCB_INFO_LEN];
    CHECK(sscb_peer_caps_to_opus(kBudsSscCaps, out) == 0);
    CHECK(memcmp(out, want, sizeof want) == 0);
    CHECK(sscb_is_opus_info(out) == 1);

    /* an Opus element is not an SSC capability */
    CHECK(sscb_peer_caps_to_opus(want, out) == SSCB_ERR_NOT_SSC);

    /* SSC element without any SSC mode bit */
    uint8_t no_ssc[SSCB_INFO_LEN];
    memcpy(no_ssc, kBudsSscCaps, sizeof no_ssc);
    no_ssc[9] = SSCB_SSC_MODE_HIFI | SSCB_SSC_MODE_UHQ_V2;
    CHECK(sscb_peer_caps_to_opus(no_ssc, out) == SSCB_ERR_UNSUPPORTED);
}

static void test_config_to_ssc(void) {
    uint8_t opus_cfg[SSCB_INFO_LEN], out[SSCB_INFO_LEN];
    sscb_build_info(SSCB_OPUS_VENDOR_ID, SSCB_OPUS_CODEC_ID, SSCB_OPUS_BRIDGE_BYTE, opus_cfg);

    /* no known SSC config byte yet: must refuse rather than guess */
    CHECK(sscb_opus_config_to_ssc(opus_cfg, SSCB_SSC_CONFIG_UNKNOWN, out) == SSCB_ERR_NO_MAPPING);
    CHECK(sscb_opus_config_to_ssc(opus_cfg, 0x100, out) == SSCB_ERR_NO_MAPPING);

    /* with an explicit byte (0x5A is an arbitrary test value, not a real SSC config) */
    CHECK(sscb_opus_config_to_ssc(opus_cfg, 0x5A, out) == 0);
    CHECK(sscb_is_ssc_info(out) == 1 && out[9] == 0x5A);

    /* only 48 kHz / 20 ms / stereo is accepted */
    uint8_t mono[SSCB_INFO_LEN];
    sscb_build_info(SSCB_OPUS_VENDOR_ID, SSCB_OPUS_CODEC_ID,
                    SSCB_OPUS_FREQ_48000 | SSCB_OPUS_FRAME_20MS | SSCB_OPUS_CH_MONO, mono);
    CHECK(sscb_opus_config_to_ssc(mono, 0x5A, out) == SSCB_ERR_UNSUPPORTED);
    uint8_t ten_ms[SSCB_INFO_LEN];
    sscb_build_info(SSCB_OPUS_VENDOR_ID, SSCB_OPUS_CODEC_ID,
                    SSCB_OPUS_FREQ_48000 | SSCB_OPUS_FRAME_10MS | SSCB_OPUS_CH_STEREO, ten_ms);
    CHECK(sscb_opus_config_to_ssc(ten_ms, 0x5A, out) == SSCB_ERR_UNSUPPORTED);

    CHECK(sscb_opus_config_to_ssc(kBudsSscCaps, 0x5A, out) == SSCB_ERR_NOT_OPUS);
}

static void test_packer_mtu(void) {
    sscb_packer p;
    /* 96 kHz UHQ at 886 kb/s: 1000-byte frame + 12-byte RTP header = 1012 > 1005.
     * This reproduces the "MTU 1012 exceeds 1005" limit reported on the forum. */
    CHECK(ssc_encoder_frame_bytes(96000, 886000) == 1000);
    CHECK(sscb_packer_init(&p, 1000, 1005, 0, 0) == SSCB_ERR_UNSUPPORTED);
    /* 96 kHz UHQ at 584 kb/s: 660 + 12 fits, one frame per packet */
    CHECK(ssc_encoder_frame_bytes(96000, 584000) == 660);
    CHECK(sscb_packer_init(&p, 660, 1005, 0, 0) == 1);
    /* 48 kHz mode at 192 kb/s: two 484-byte frames + 12 = 980 fit */
    CHECK(sscb_packer_init(&p, 484, 1005, 0, 0) == 2);
    CHECK(sscb_packer_init(&p, 484, 1005, 1, 0) == 1); /* max_frames caps it */
    CHECK(sscb_packer_init(&p, 0, 1005, 0, 0) == SSCB_ERR_ARG);
}

static void test_packer_payload(void) {
    sscb_packer p;
    uint8_t frames[2 * 484], out[1024];
    uint32_t ts = 0;
    memset(frames, 0xAB, sizeof frames);
    CHECK(sscb_packer_init(&p, 484, 1005, 0, 1000) == 2);
    CHECK(sscb_packer_payload(&p, frames, 2, out, sizeof out, &ts) == 968);
    CHECK(ts == 1000);
    CHECK(sscb_packer_payload(&p, frames, 1, out, sizeof out, &ts) == 484);
    CHECK(ts == 1000 + 2 * 864);
    CHECK(p.timestamp == 1000 + 3 * 864);
    CHECK(sscb_packer_payload(&p, frames, 3, out, sizeof out, &ts) == SSCB_ERR_ARG); /* > frames_per_packet */
    CHECK(sscb_packer_payload(&p, frames, 2, out, 900, &ts) == SSCB_ERR_BUFFER);
}

/* Encodes a real test vector with the open-source encoder and packs it as the bridge would. */
static void test_end_to_end(const char *vectors) {
    char path[1024];
    FILE *f;
    int16_t pcm[2 * SSC_ENC_FRAME_SAMPLES];
    uint8_t frames[2 * SSC_ENC_MAX_FRAME_BYTES], payload[2048];
    ssc_encoder *enc;
    sscb_packer p;
    int packets = 0, pending = 0, frame_bytes;
    uint32_t ts, last_ts = 0;

    snprintf(path, sizeof path, "%s/mix48k.s16le", vectors);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "end-to-end: cannot open %s\n", path);
        failures++;
        return;
    }
    enc = ssc_encoder_create(48000);
    CHECK(enc != NULL);
    frame_bytes = ssc_encoder_frame_bytes(48000, 192000);
    CHECK(frame_bytes == 484);
    CHECK(sscb_packer_init(&p, frame_bytes, 1005, 0, 0) == 2);

    while (fread(pcm, sizeof pcm, 1, f) == 1) {
        int n = ssc_encode(enc, pcm, 192000, 0, frames + pending * frame_bytes, SSC_ENC_MAX_FRAME_BYTES);
        CHECK(n == frame_bytes);
        CHECK(frames[pending * frame_bytes] == 0xFF); /* SSC frame header starts with FF Ex */
        CHECK((frames[pending * frame_bytes + 1] & 0xF0) == 0xE0);
        if (++pending == p.frames_per_packet) {
            int len = sscb_packer_payload(&p, frames, pending, payload, sizeof payload, &ts);
            CHECK(len == 2 * frame_bytes);
            CHECK(len + SSCB_RTP_HEADER_BYTES <= 1005);
            if (packets > 0) CHECK(ts - last_ts == 2 * SSCB_FRAME_SAMPLES);
            last_ts = ts;
            packets++;
            pending = 0;
        }
    }
    fclose(f);
    ssc_encoder_destroy(enc);
    CHECK(packets > 50);
    printf("end-to-end: %d packets of %d bytes (2 x %d-byte frames) at 192 kb/s, 48 kHz\n", packets,
           2 * frame_bytes, frame_bytes);
}

int main(int argc, char **argv) {
    test_ids();
    test_caps_to_opus();
    test_config_to_ssc();
    test_packer_mtu();
    test_packer_payload();
    if (argc > 1) test_end_to_end(argv[1]);
    else printf("end-to-end: skipped (no vectors directory given)\n");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL BRIDGE TESTS PASSED\n");
    return 0;
}
