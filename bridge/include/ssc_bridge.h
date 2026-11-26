/* ssc_bridge: pure data helpers for carrying SSC over the A2DP stack's Opus codec slot.
 *
 * No Android or hooking code here: only codec-information translation and payload packing,
 * so everything can be unit-tested on a PC.
 *
 * Codec information element layout, as the AOSP A2DP stack stores it in its codec_info arrays:
 *   [0]     LOSC: number of bytes that follow (9 for both SSC and Opus)
 *   [1]     media type (audio = 0x00)
 *   [2]     media codec type (0xFF = vendor specific)
 *   [3..6]  vendor id, little endian
 *   [7..8]  codec id, little endian
 *   [9]     one vendor-specific byte (SSC: capability/mode bits, Opus: config bits)
 */
#ifndef SSC_BRIDGE_H
#define SSC_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSCB_INFO_LEN 10
#define SSCB_LOSC 9
#define SSCB_MEDIA_TYPE_AUDIO 0x00
#define SSCB_CODEC_TYPE_VENDOR 0xFF

/* Samsung SSC: vendor 0x00000075, codec 0x0103 (seen in the Buds 3 Pro capability on a Xiaomi 13). */
#define SSCB_SSC_VENDOR_ID 0x00000075u
#define SSCB_SSC_CODEC_ID 0x0103u
/* AOSP Opus vendor codec (a2dp_vendor_opus_constants.h). */
#define SSCB_OPUS_VENDOR_ID 0x000000E0u
#define SSCB_OPUS_CODEC_ID 0x0001u

/* Opus config bits (AOSP a2dp_vendor_opus_constants.h). */
#define SSCB_OPUS_CH_MONO 0x01
#define SSCB_OPUS_CH_STEREO 0x02
#define SSCB_OPUS_CH_DUAL_MONO 0x04
#define SSCB_OPUS_CH_MASK 0x07
#define SSCB_OPUS_FRAME_10MS 0x08
#define SSCB_OPUS_FRAME_20MS 0x10
#define SSCB_OPUS_FRAME_MASK 0x18
#define SSCB_OPUS_FREQ_48000 0x80
#define SSCB_OPUS_FREQ_MASK 0x80

/* The only Opus configuration the bridge offers the stack: 48 kHz, 20 ms, stereo (= 0x92). */
#define SSCB_OPUS_BRIDGE_BYTE (SSCB_OPUS_FREQ_48000 | SSCB_OPUS_FRAME_20MS | SSCB_OPUS_CH_STEREO)

/* SSC mode bits as described by the third-party samsung-ssc-decoder README. NOT verified against
 * Samsung; the Buds 3 Pro reported 0x3E (= SSC bits + HiFi + UHQ v2) to a Xiaomi 13. */
#define SSCB_SSC_MODE_LEGACY_UHQ 0x80
#define SSCB_SSC_MODE_SSC_MASK 0x38
#define SSCB_SSC_MODE_HIFI 0x04
#define SSCB_SSC_MODE_UHQ_V2 0x02

/* SSC configuration bytes a Galaxy S25+ sent to Galaxy Buds 3 Pro (Set Configuration / Reconfigure),
 * captured with btsnoop and matched to the bitrate codes of the frames that followed:
 *   0x0C -> 48 kHz "HiFi", frames at 192 kb/s (rate code 0xF3)
 *   0x0A -> 96 kHz UHQ,    frames at 584 kb/s (rate code 0xE3) */
#define SSCB_SSC_CONFIG_HIFI_48K 0x0C
#define SSCB_SSC_CONFIG_UHQ_96K 0x0A
/* Use for any mode whose configuration byte has not been captured: the translation then refuses. */
#define SSCB_SSC_CONFIG_UNKNOWN (-1)

/* RTP header added by the AVDTP layer in front of every media payload. */
#define SSCB_RTP_HEADER_BYTES 12
/* PCM samples per channel in one SSC frame (= RTP timestamp step per frame). */
#define SSCB_FRAME_SAMPLES 864

/* Error codes (negative). */
#define SSCB_ERR_ARG (-1)
#define SSCB_ERR_NOT_SSC (-2)
#define SSCB_ERR_NOT_OPUS (-3)
#define SSCB_ERR_UNSUPPORTED (-4)
#define SSCB_ERR_BUFFER (-5)
#define SSCB_ERR_NO_MAPPING (-6)

/* Reads vendor and codec id from a vendor codec info element. Returns 0, or SSCB_ERR_ARG if the
 * element is not a 9-byte audio vendor element. */
int sscb_info_get_ids(const uint8_t *info, uint32_t *vendor_id, uint16_t *codec_id);

/* 1 if info is an SSC element (vendor 0x75, codec 0x0103), else 0. */
int sscb_is_ssc_info(const uint8_t *info);
/* 1 if info is an Opus element (vendor 0xE0, codec 0x0001), else 0. */
int sscb_is_opus_info(const uint8_t *info);

/* Writes a 10-byte vendor element. */
void sscb_build_info(uint32_t vendor_id, uint16_t codec_id, uint8_t value, uint8_t out[SSCB_INFO_LEN]);

/* Peer -> stack: turns the SSC capability the earbuds report into the Opus capability the stack
 * understands (always SSCB_OPUS_BRIDGE_BYTE). Returns 0, SSCB_ERR_NOT_SSC, or SSCB_ERR_UNSUPPORTED
 * when the capability has none of the SSC mode bits. */
int sscb_peer_caps_to_opus(const uint8_t *ssc_info, uint8_t out[SSCB_INFO_LEN]);

/* Stack -> peer: turns the Opus configuration the stack chose back into an SSC configuration.
 * ssc_config_byte is the value to send to the earbuds; SSCB_SSC_CONFIG_UNKNOWN (or any negative
 * value) makes the call fail with SSCB_ERR_NO_MAPPING instead of guessing.
 * Returns 0, SSCB_ERR_NOT_OPUS, SSCB_ERR_UNSUPPORTED (not 48 kHz / 20 ms / stereo) or SSCB_ERR_NO_MAPPING. */
int sscb_opus_config_to_ssc(const uint8_t *opus_info, int ssc_config_byte, uint8_t out[SSCB_INFO_LEN]);

/* Packs whole SSC frames into AVDTP media payloads (the RTP header is added by AVDTP, not here). */
typedef struct {
    int frame_bytes;       /* size of every frame at the current bitrate */
    int frames_per_packet; /* frames placed in one payload */
    uint32_t timestamp;    /* RTP timestamp of the next payload, in samples */
} sscb_packer;

/* max_packet_bytes: the AVDTP media MTU, RTP header included. max_frames: upper bound per packet
 * (0 = no bound). Returns frames per packet (>= 1), SSCB_ERR_ARG, or SSCB_ERR_UNSUPPORTED when one
 * frame plus the RTP header does not fit in max_packet_bytes. */
int sscb_packer_init(sscb_packer *p, int frame_bytes, int max_packet_bytes, int max_frames, uint32_t ts0);

/* Copies nframes frames (1..frames_per_packet) into out and returns the payload size.
 * *ts receives the RTP timestamp of this payload; the packer then advances by nframes * 864.
 * Returns SSCB_ERR_ARG or SSCB_ERR_BUFFER on error. */
int sscb_packer_payload(sscb_packer *p, const uint8_t *frames, int nframes, uint8_t *out, int out_cap,
                        uint32_t *ts);

#ifdef __cplusplus
}
#endif
#endif
