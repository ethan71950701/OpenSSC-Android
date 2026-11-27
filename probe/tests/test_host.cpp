#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "guard.h"
#include "symres.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    CHECK(sr_symbol_matches("avdt_msg_prs_cfg", "avdt_msg_prs_cfg"));
    CHECK(sr_symbol_matches("avdt_msg_prs_cfg.llvm.123", "avdt_msg_prs_cfg"));
    CHECK(!sr_symbol_matches("avdt_msg_prs_cfg.cold", "avdt_msg_prs_cfg"));
    /* exact mangled names (constructors) */
    CHECK(sr_symbol_matches("_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_t", "_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_t"));
    CHECK(!sr_symbol_matches("_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_tx", "_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_t"));
    CHECK(!sr_symbol_matches("_ZN15A2dpCodecConfigC1E23btav_a2dp_codec_index_t", "_ZN15A2dpCodecConfigC2E23btav_a2dp_codec_index_t"));
    CHECK(!sr_symbol_matches("avdt_msg_prs_cfg2", "avdt_msg_prs_cfg"));
    CHECK(sr_symbol_matches("_Z16avdt_msg_prs_cfgP1Ah.llvm.9", "avdt_msg_prs_cfg"));
    CHECK(sr_symbol_matches("_ZL16avdt_msg_prs_cfgv", "avdt_msg_prs_cfg"));
    CHECK(sr_symbol_matches("_ZN12_GLOBAL__N_116avdt_msg_prs_cfgEv", "avdt_msg_prs_cfg"));
    CHECK(sr_symbol_matches("_ZNK1A1BEv", "A::B"));
    /* qualified names with 3+ components, as in the real Xiaomi symbol table */
    const char *fe = "_ZN9bluetooth5audio4a2dp17is_hal_offloadingEv";
    const char *aidl = "_ZN9bluetooth5audio4aidl4a2dp17is_hal_offloadingEv";
    CHECK(sr_symbol_matches(fe, "audio::a2dp::is_hal_offloading"));
    CHECK(!sr_symbol_matches(aidl, "audio::a2dp::is_hal_offloading"));
    CHECK(sr_symbol_matches(aidl, "aidl::a2dp::is_hal_offloading"));
    CHECK(sr_symbol_matches(fe, "a2dp::is_hal_offloading") && sr_symbol_matches(aidl, "a2dp::is_hal_offloading"));
    CHECK(sr_symbol_matches(fe, "bluetooth::audio::a2dp::is_hal_offloading"));
    CHECK(!sr_symbol_matches(fe, "x::bluetooth::audio::a2dp::is_hal_offloading"));
    CHECK(!sr_symbol_matches(fe, "audio::a2dp::") && !sr_symbol_matches(fe, "::a2dp") && !sr_symbol_matches(fe, ""));
    CHECK(sr_symbol_matches("_ZN7BtaAvCo13SetActivePeerERK10RawAddressh", "BtaAvCo::SetActivePeer"));
    CHECK(!sr_symbol_matches("_ZN12BtifAvSource13SetActivePeerERK10RawAddressNSt3__17promiseIvEE", "BtaAvCo::SetActivePeer"));
    CHECK(sr_symbol_matches("_ZN12BtifAvSource13SetActivePeerERK10RawAddressNSt3__17promiseIvEE", "BtifAvSource::SetActivePeer"));
    CHECK(sr_symbol_matches("_Z30btif_a2dp_source_start_sessionRK10RawAddressNSt3__17promiseIvEE", "btif_a2dp_source_start_session"));
    CHECK(!sr_symbol_matches("_ZL38btif_a2dp_source_start_session_delayedRK10RawAddress", "btif_a2dp_source_start_session"));
    CHECK(!sr_symbol_matches("btif", "a::btif"));
    CHECK(!sr_symbol_matches("_ZZ16avdt_msg_prs_cfgvENK3$_0clEv", "avdt_msg_prs_cfg"));
    CHECK(!sr_symbol_matches("_Z16avdt_msg_prs_cfg", "avdt_msg_prs_cfg"));
    CHECK(!sr_symbol_matches("_Z99999999999999999999x", "x"));
    CHECK(!sr_symbol_matches("_Z50short", "short"));
    CHECK(!sr_symbol_matches("_ZN", "x"));

    FILE *f = fopen(argv[1], "rb"); CHECK(f);
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *d = (uint8_t *)malloc(n); CHECK(fread(d, 1, n, f) == (size_t)n); fclose(f);
    sr_target t[] = {{"avdt_msg_prs_cfg", 1}, {"avdt_msg_bld_cfg", 1}, {"a2dp_vendor_opus_send_frames", 1},
                     {"A2dpCodecConfigOpusBase::setCodecConfig", 0}, {"a2dp_vendor_opus_encoder_init", 1},
                     {"setCodecConfig", 0}, {"does_not_exist", 0}};
    CHECK(sr_scan(d, n, t, 7) == SR_OK);
    for (int i = 0; i < 5; i++) { printf("%s -> %d %s\n", t[i].name, t[i].matches, t[i].match[0].symbol); CHECK(t[i].matches == 1); }
    CHECK(t[5].matches == 2);
    CHECK(t[6].matches == 0);
    /* damaged input must return an error or fewer matches, never read out of bounds (ASan) */
    CHECK(sr_scan(d, 0, t, 7) == SR_BAD_ELF);
    for (long cut = 1; cut < n; cut += 37) {
        uint8_t *c = (uint8_t *)malloc(cut); memcpy(c, d, cut); sr_scan(c, cut, t, 7); free(c);
    }
    srand(1);
    for (int it = 0; it < 20000; it++) {
        uint8_t *c = (uint8_t *)malloc(n); memcpy(c, d, n);
        for (int k = 0; k < 1 + rand() % 8; k++) c[rand() % n] ^= (uint8_t)(1 + rand() % 255);
        sr_scan(c, n, t, 7); free(c);
    }
    free(d);

    char dir[] = "/tmp/guardXXXXXX"; CHECK(mkdtemp(dir)); char p[600]; snprintf(p, sizeof p, "%s/s", dir);
    CHECK(guard_begin(p) == 1); CHECK(guard_begin(p) == 2); CHECK(guard_begin(p) == 3);
    CHECK(guard_begin(p) == 0); CHECK(guard_begin(p) == 0);
    CHECK(guard_success(p) == 0); CHECK(guard_begin(p) == 1);
    f = fopen(p, "w"); fputs("junk", f); fclose(f); CHECK(guard_begin(p) == -1);
    CHECK(guard_begin("/nonexistent/dir/s") == -1);
    printf(fails ? "%d FAILED\n" : "ALL TESTS PASSED\n", fails);
    return fails != 0;
}
