/* Wire-level SSC <-> Opus rewriting of AVDTP service-capability lists. Pure functions on byte buffers,
 * built on bridge/ (ssc_bridge.h); no Android or hooking code, so they are unit-tested on the host.
 *
 * An AVDTP capability/configuration list is a sequence of: category (1 byte), length (1 byte), payload.
 * The Media Codec category (0x07) with length 9 carries the codec element; the bridge's "info" array
 * starts at that length byte (info[0] = LOSC = 9). Other categories are skipped untouched. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define TR_CAT_MEDIA_CODEC 0x07
#define TR_SSC_CONFIG_BYTE 0x0C /* HiFi 48 kHz, captured on the S25+ */

/* Receive side: rewrites every SSC capability element in [p, p+len) into the bridge's Opus capability.
 * Returns the number of elements rewritten. Truncated or malformed lists are left as they are. */
int tr_caps_ssc_to_opus(uint8_t *p, size_t len);

/* Send side: rewrites every Opus configuration element in [p, p+len) (48 kHz / 20 ms / stereo) into the
 * SSC configuration `ssc_config_byte`. Returns the number rewritten. *other_codec (if not null) is set
 * to 1 when the list holds a Media Codec element that is not Opus (so the config is for another codec). */
int tr_config_opus_to_ssc(uint8_t *p, size_t len, int ssc_config_byte, int *other_codec);

/* Receive side, test aid: marks every audio AAC capability element (codec type 0x02) in [p, p+len) as an
 * invalid codec type (0x0E), so the stack ignores that endpoint and falls back to the next best codec. Used
 * only after an SSC peer was seen, because the stack ranks AAC above Opus. Returns the number changed. */
int tr_hide_aac(uint8_t *p, size_t len);
