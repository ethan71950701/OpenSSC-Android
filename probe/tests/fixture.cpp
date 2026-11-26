#include <stdint.h>
volatile int sink;
#define KEEP __attribute__((noinline, used))
struct AvdtpSepConfig { int x; };
static KEEP void avdt_msg_prs_cfg(AvdtpSepConfig *c, uint8_t *p) { sink = c->x + *p; }
KEEP void avdt_msg_bld_cfg(uint8_t **p, AvdtpSepConfig *c) { sink = **p + c->x; }
namespace bluetooth { namespace a2dp { KEEP void a2dp_vendor_opus_send_frames(uint64_t t) { sink = (int)t; } } }
class A2dpCodecConfigOpusBase { public: KEEP bool setCodecConfig(const uint8_t *p) const; };
bool A2dpCodecConfigOpusBase::setCodecConfig(const uint8_t *p) const { return *p; }
class Other { public: KEEP void setCodecConfig(int v); };
void Other::setCodecConfig(int v) { sink = v; }
extern "C" KEEP void a2dp_vendor_opus_encoder_init(int v) { sink = v; }
extern "C" KEEP void a2dp_vendor_opus_encoder_init_extra(int v) { sink = v + 1; }
extern "C" void *entry(int v) {
    AvdtpSepConfig c{v}; uint8_t b = (uint8_t)v; uint8_t *pb = &b;
    avdt_msg_prs_cfg(&c, &b); avdt_msg_bld_cfg(&pb, &c);
    return nullptr;
}
