/* Stage 0.4: real SSC audio in place of the muted Opus frames.
 *
 * Runs on the stack's A2DP media thread, called from the hooked Opus send_frames every encoder interval
 * (20 ms). It reads 48 kHz / 16-bit / stereo PCM through the stack's read callback, encodes 864-sample SSC
 * frames (192 kb/s "HiFi", matching configuration byte 0x0C), packs them one frame per packet (frames only, no
 * media payload header; the stack adds the 12-byte RTP header) and hands each packet to the stack's enqueue
 * callback as a BT_HDR, exactly like the Opus encoder it replaces.
 *
 * No Android dependency: allocation, PCM source and packet sink are passed in, so it runs in host tests. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef uint32_t (*sscs_read_fn)(uint8_t *buf, uint32_t len);
typedef bool (*sscs_enqueue_fn)(void *bt_hdr, size_t frames, uint32_t bytes_read);
typedef void *(*sscs_alloc_fn)(size_t size);

#define SSCS_SAMPLE_RATE 48000
#define SSCS_BITRATE 192000
#define SSCS_BUF_SIZE 4112  /* BT_DEFAULT_BUFFER_SIZE, as the Opus encoder allocates */
#define SSCS_BUF_OFFSET 24  /* data offset the Opus encoder uses (room for AVDTP/L2CAP headers) */

/* Experiment knobs (0.4.2), applied by the next sscs_start. ts_step: RTP timestamp advance per frame (default 864,
 * negative keeps the default). max_fpp: frames per packet 1..4, still limited by the MTU (0 or out of range: default 1). */
void sscs_set_options(int ts_step, int max_fpp);

/* Prepares a stream. Returns frames per packet (>= 1) or a negative value (then nothing is sent). */
int sscs_start(sscs_read_fn read, sscs_enqueue_fn enqueue, sscs_alloc_fn alloc, uint16_t peer_mtu);
/* Drops buffered PCM and partially filled packets (feeding reset / flush). */
void sscs_reset(void);
/* One encoder tick. now_us: the stack's timestamp argument (microseconds). Returns packets enqueued (>= 0),
 * or -1 if the sender is not started. */
int sscs_tick(uint64_t now_us);
/* Ends the stream; the encoder is kept for the next one. */
void sscs_stop(void);

struct sscs_stats {
    unsigned ticks, frames, packets, underflows, enqueue_failed;
    int frame_bytes, frames_per_packet, ts_step;
};
void sscs_get_stats(sscs_stats *out);

/* Level trace for diagnosis: one character per encoded frame (input PCM peak, log scale: ' ' silent ... '9' full
 * scale). sscs_take_trace copies what has accumulated (up to cap-1 chars, NUL-terminated) and clears it. */
int sscs_take_trace(char *out, int cap);
