#include "ssc_bridge.h"

#include <string.h>

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int sscb_info_get_ids(const uint8_t *info, uint32_t *vendor_id, uint16_t *codec_id) {
    if (!info || info[0] != SSCB_LOSC || (info[1] >> 4) != SSCB_MEDIA_TYPE_AUDIO ||
        info[2] != SSCB_CODEC_TYPE_VENDOR)
        return SSCB_ERR_ARG;
    if (vendor_id) *vendor_id = rd32le(info + 3);
    if (codec_id) *codec_id = (uint16_t)(info[7] | (info[8] << 8));
    return 0;
}

static int has_ids(const uint8_t *info, uint32_t vendor, uint16_t codec) {
    uint32_t v;
    uint16_t c;
    return sscb_info_get_ids(info, &v, &c) == 0 && v == vendor && c == codec;
}

int sscb_is_ssc_info(const uint8_t *info) { return has_ids(info, SSCB_SSC_VENDOR_ID, SSCB_SSC_CODEC_ID); }

int sscb_is_opus_info(const uint8_t *info) { return has_ids(info, SSCB_OPUS_VENDOR_ID, SSCB_OPUS_CODEC_ID); }

void sscb_build_info(uint32_t vendor_id, uint16_t codec_id, uint8_t value, uint8_t out[SSCB_INFO_LEN]) {
    out[0] = SSCB_LOSC;
    out[1] = SSCB_MEDIA_TYPE_AUDIO << 4;
    out[2] = SSCB_CODEC_TYPE_VENDOR;
    out[3] = (uint8_t)vendor_id;
    out[4] = (uint8_t)(vendor_id >> 8);
    out[5] = (uint8_t)(vendor_id >> 16);
    out[6] = (uint8_t)(vendor_id >> 24);
    out[7] = (uint8_t)codec_id;
    out[8] = (uint8_t)(codec_id >> 8);
    out[9] = value;
}

int sscb_peer_caps_to_opus(const uint8_t *ssc_info, uint8_t out[SSCB_INFO_LEN]) {
    if (!out) return SSCB_ERR_ARG;
    if (!sscb_is_ssc_info(ssc_info)) return SSCB_ERR_NOT_SSC;
    if (!(ssc_info[9] & SSCB_SSC_MODE_SSC_MASK)) return SSCB_ERR_UNSUPPORTED;
    sscb_build_info(SSCB_OPUS_VENDOR_ID, SSCB_OPUS_CODEC_ID, SSCB_OPUS_BRIDGE_BYTE, out);
    return 0;
}

int sscb_opus_config_to_ssc(const uint8_t *opus_info, int ssc_config_byte, uint8_t out[SSCB_INFO_LEN]) {
    if (!out) return SSCB_ERR_ARG;
    if (!sscb_is_opus_info(opus_info)) return SSCB_ERR_NOT_OPUS;
    if ((opus_info[9] & SSCB_OPUS_FREQ_MASK) != SSCB_OPUS_FREQ_48000 ||
        (opus_info[9] & SSCB_OPUS_FRAME_MASK) != SSCB_OPUS_FRAME_20MS ||
        (opus_info[9] & SSCB_OPUS_CH_MASK) != SSCB_OPUS_CH_STEREO)
        return SSCB_ERR_UNSUPPORTED;
    if (ssc_config_byte < 0 || ssc_config_byte > 0xFF) return SSCB_ERR_NO_MAPPING;
    sscb_build_info(SSCB_SSC_VENDOR_ID, SSCB_SSC_CODEC_ID, (uint8_t)ssc_config_byte, out);
    return 0;
}

int sscb_packer_init(sscb_packer *p, int frame_bytes, int max_packet_bytes, int max_frames, uint32_t ts0) {
    int fpp;
    if (!p || frame_bytes <= 0 || max_packet_bytes <= 0 || max_frames < 0) return SSCB_ERR_ARG;
    fpp = (max_packet_bytes - SSCB_RTP_HEADER_BYTES) / frame_bytes;
    if (fpp < 1) return SSCB_ERR_UNSUPPORTED;
    if (max_frames > 0 && fpp > max_frames) fpp = max_frames;
    p->frame_bytes = frame_bytes;
    p->frames_per_packet = fpp;
    p->timestamp = ts0;
    return fpp;
}

int sscb_packer_payload(sscb_packer *p, const uint8_t *frames, int nframes, uint8_t *out, int out_cap,
                        uint32_t *ts) {
    int bytes;
    if (!p || !frames || !out || nframes < 1 || nframes > p->frames_per_packet) return SSCB_ERR_ARG;
    bytes = nframes * p->frame_bytes;
    if (bytes > out_cap) return SSCB_ERR_BUFFER;
    memcpy(out, frames, (size_t)bytes);
    if (ts) *ts = p->timestamp;
    p->timestamp += (uint32_t)(nframes * SSCB_FRAME_SAMPLES);
    return bytes;
}
