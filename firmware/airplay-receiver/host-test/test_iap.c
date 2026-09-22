// Host-side test for iap.c: scripts a car conversation, verifies responses.
// cc test_iap.c ../components/ipod_usb/iap.c -Istubs \
//    -I../components/ipod_usb/include -o test_iap && ./test_iap
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "iap.h"

int64_t fake_us;
int64_t esp_timer_get_time(void) { return fake_us; }

// Deterministic RNG replacement verifies exact challenge serialization.
void esp_fill_random(void *buffer, size_t length) {
    assert(length == 20);
    for (size_t i = 0; i < length; i++) ((uint8_t *)buffer)[i] = 0x80 + i;
}

// TX capture: reports as sent via tud_hid_report.
static uint8_t cap_id[64];
static uint8_t cap_data[64][64];
static uint8_t cap_len[64];
static int cap_n;

void ipod_usb_log(const char *fmt, ...) {
    // quiet in tests (verified through TX decoding instead)
    (void) fmt;
}
static unsigned probe_reconnects;
void ipod_usb_request_probe_next(void) { probe_reconnects++; }

bool tud_hid_report(uint8_t report_id, void const *report, uint16_t len) {
    const uint8_t descriptor_lengths[] = {0, 12, 14, 20, 63};
    assert(report_id >= 1 && report_id <= 4);
    assert(len == descriptor_lengths[report_id]);
    const uint8_t *bytes = report;
    if (bytes[0] == 0 && bytes[1] == 0x55 && bytes[2] != 0) {
        unsigned end = 1 + 3 + bytes[2]; // link + sync/length/checksum + payload
        assert(end <= len);
        for (unsigned i = end; i < len; i++) assert(bytes[i] == 0);
    }
    if (cap_n >= 64 || len > 64) return false;
    cap_id[cap_n] = report_id;
    memcpy(cap_data[cap_n], report, len);
    cap_len[cap_n] = (uint8_t) len;
    cap_n++;
    return true;
}

// ---- builders (car side) ----
static uint8_t fbuf[2048];
static int fn;

static void pkt_begin(void) { fn = 0; fbuf[fn++] = 0x55; }
static void pkt_cmd(const uint8_t *cmd, int n) {
    if (n > 256) { fbuf[fn++] = 0; fbuf[fn++] = (n >> 8) & 0xFF; fbuf[fn++] = n & 0xFF; }
    else fbuf[fn++] = (uint8_t) n;
    memcpy(fbuf + fn, cmd, n);
    fn += n;
    uint8_t s = 0;
    for (int i = 1; i < fn; i++) s += fbuf[i];
    fbuf[fn++] = (uint8_t) -s;
}

// Feed frame as HID reports with chaining (report id 5, payload <= 7+1).
static void feed_frame(const uint8_t *f, int n) {
    int off = 0;
    while (n > 0) {
        int chunk = n > 7 ? 7 : n;
        uint8_t link = 0x00;
        if (n > 7) link = (off == 0) ? 0x02 : 0x03;
        else if (off > 0) link = 0x01;
        uint8_t rep[8] = { (uint8_t) link, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(rep + 1, f + off, chunk);
        iap_rx_report(5, rep, (uint16_t) (chunk + 1));
        off += chunk;
        n -= chunk;
    }
    iap_tx_pump();
}

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

// Decode captured TX into packets: returns count, fills lingos/cmds/payloads.
static int tx_cmds[32];
static uint8_t tx_pay[32][256];
static int tx_paylen[32];
static int tx_has_trx[32];

static int decode_tx(void) {
    // reassemble frames by chaining
    static uint8_t frame[2048];
    int ncmd = 0, flen = 0;
    for (int i = 0; i < cap_n; i++) {
        uint8_t link = cap_data[i][0];
        if (link == 0x00 || link == 0x02) flen = 0;
        memcpy(frame + flen, cap_data[i] + 1, cap_len[i] - 1);
        flen += cap_len[i] - 1;
        if (link == 0x00 || link == 0x01) {
            // parse packets
            int o = 0;
            while (o < flen) {
                while (o < flen && frame[o] != 0x55) o++;
                if (o >= flen) break;
                o++;
                if (o >= flen) break;
                int plen, hdr;
                if (frame[o] == 0) { plen = (frame[o+1] << 8) | frame[o+2]; hdr = 3; }
                else { plen = frame[o]; hdr = 1; }
                if (o + hdr + plen + 1 > flen) break;
                uint8_t s = 0;
                for (int k = 0; k < hdr + plen + 1; k++) s += frame[o + k];
                CHECK(s == 0, "TX checksum bad");
                const uint8_t *p = frame + o + hdr;
                int lingo = p[0], cmd, h = 1;
                if (lingo == 0x04) { cmd = (p[1] << 8) | p[2]; h = 3; }
                else cmd = p[1], h = 2;
                tx_cmds[ncmd] = (lingo << 16) | cmd;
                // strip trx heuristically: try known fixed sizes? keep raw, note trx presence via test
                tx_has_trx[ncmd] = 0;
                memcpy(tx_pay[ncmd], p + h, plen - h);
                tx_paylen[ncmd] = plen - h;
                ncmd++;
                o += hdr + plen + 1;
            }
            flen = 0;
        }
    }
    cap_n = 0;
    return ncmd;
}

static void show_tx(const char *label) {
    int n = decode_tx();
    printf("-- %s: %d cmds --\n", label, n);
    for (int i = 0; i < n; i++) {
        printf("   lingo=%02X cmd=%04X len=%d : ", (tx_cmds[i] >> 16) & 0xFF, tx_cmds[i] & 0xFFFF, tx_pay[i] ? tx_paylen[i] : 0);
        for (int j = 0; j < tx_paylen[i] && j < 24; j++) printf("%02X ", tx_pay[i][j]);
        printf("\n");
    }
}

int main(void) {
    iap_set_serial("68EE8F5B3D34");

    // Multipart v2 auth: transaction fidelity, retries and strict audio gate.
    {
        const uint8_t start[] = {0, 0x38};
        pkt_begin(); pkt_cmd(start, sizeof(start)); feed_frame(fbuf, fn); decode_tx();
        const uint8_t first[] = {0, 0x15, 0, 0x41, 2, 0, 0, 2, 0xaa};
        pkt_begin(); pkt_cmd(first, sizeof(first)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 2, "first cert only ACK");
        const uint8_t final[] = {0, 0x15, 0, 0x44, 2, 0, 2, 2, 0xcc};
        pkt_begin(); pkt_cmd(final, sizeof(final)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 2 && tx_pay[0][2] == 2,
              "missing middle cert rejected");
        pkt_begin(); pkt_cmd(first, sizeof(first)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_pay[0][2] == 0, "duplicate intermediate ACK");
        const uint8_t middle[] = {0, 0x15, 0, 0x42, 2, 0, 1, 2, 0xbb};
        pkt_begin(); pkt_cmd(middle, sizeof(middle)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 2, "middle cert only ACK");
        pkt_begin(); pkt_cmd(final, sizeof(final)); feed_frame(fbuf, fn);
        CHECK(cap_n == 2 && cap_id[0] == 1 && cap_id[1] == 1,
              "final cert sends auth ACK then non-IDPS AccessoryInfo");
        const uint8_t info[] = {0, 0x28, 0, 0x45, 0, 0, 0, 2, 1};
        pkt_begin(); pkt_cmd(info, sizeof(info)); feed_frame(fbuf, fn);
        iap_tx_pump();
        CHECK(cap_n == 3 && cap_id[0] == 1 && cap_id[1] == 1 && cap_id[2] == 4,
              "signature sequence uses two report1 packets then one report4");
        CHECK(decode_tx() == 3 && tx_cmds[0] == 0x16 && tx_cmds[1] == 0x27 &&
              tx_cmds[2] == 0x17,
              "R36 non-IDPS authentication command order");
        CHECK(tx_paylen[2] == 23 && tx_pay[2][0] == 0 && tx_pay[2][1] == 0x44,
              "signature response preserves final certificate transaction");
        for (int i=2; i<22; i++) CHECK(tx_pay[2][i] == 0x80 + i - 2, "v2 challenge byte %d", i);
        CHECK(tx_pay[2][22] == 0, "oandrew Auth 2.x initial counter 0");
        iap_snapshot_t snap; iap_snapshot(&snap);
        CHECK(!strcmp(snap.state, "AUTH_SIG"), "wait for signature, not next certificate");
        pkt_begin(); pkt_cmd(final, sizeof(final)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 0x16, "duplicate final doesn't restart negotiation");
        const uint8_t sig[] = {0, 0x18, 0, 0x45, 0x12, 0x34};
        pkt_begin(); pkt_cmd(sig, sizeof(sig)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 2 && tx_cmds[0] == 0x19 && tx_cmds[1] == 0x0a0002,
              "signature ACK precedes single DigitalAudio start");
        CHECK(tx_pay[0][0] == 0 && tx_pay[0][1] == 0x45,
              "signature ACK transaction is 0045, not fabricated 0000");
        pkt_begin(); pkt_cmd(sig, sizeof(sig)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 0x19, "duplicate signature doesn't restart audio");
        const uint8_t audio_ack[] = {0x0a, 0, 0, 0x46, 0, 4};
        pkt_begin(); pkt_cmd(audio_ack, sizeof(audio_ack)); feed_frame(fbuf, fn); decode_tx();
        iap_snapshot(&snap); CHECK(!strcmp(snap.state, "READY"), "audio accepted ready");
        iap_reset_protocol();
        iap_snapshot(&snap); CHECK(!strcmp(snap.state, "IDLE"), "new USB session reset");
        const uint8_t bad[] = {0, 0x15, 2, 0, 0};
        pkt_begin(); pkt_cmd(bad, sizeof(bad)); feed_frame(fbuf, fn);
        CHECK(decode_tx() == 1 && tx_cmds[0] == 2 && tx_pay[0][0] == 2,
              "truncated v2 cert rejected without starting audio");
        iap_reset_protocol();
    }

    // Volvo's captured legacy flow has no transaction IDs and eight sections.
    {
        for (unsigned section = 0; section < 8; section++) {
            uint8_t cert[134] = {0, 0x15, 2, 0, section, 7};
            unsigned length = section == 7 ? 55 : sizeof(cert);
            memset(cert + 6, 0xa5, length - 6);
            pkt_begin(); pkt_cmd(cert, length); feed_frame(fbuf, fn);
            if (section == 7) {
                CHECK(cap_n == 2 && cap_id[0] == 1 && cap_id[1] == 1,
                      "legacy final cert sends auth ACK then AccessoryInfo");
                const uint8_t info[] = {0, 0x28, 0, 0, 0, 2, 1};
                pkt_begin(); pkt_cmd(info, sizeof(info)); feed_frame(fbuf, fn);
                iap_tx_pump();
            }
            bool transport_ok = cap_n == 3 && cap_id[0] == 1 && cap_id[1] == 1 &&
                                cap_id[2] == 4;
            int count = decode_tx();
            if (section < 7) {
                CHECK(count == 1 && tx_cmds[0] == 2 && tx_paylen[0] == 2 &&
                      tx_pay[0][0] == 0 && tx_pay[0][1] == 0x15, "legacy section ACK");
            } else {
                CHECK(transport_ok, "legacy signature challenge uses one report4");
                CHECK(count == 3 && tx_cmds[0] == 0x16 && tx_cmds[1] == 0x27 &&
                      tx_cmds[2] == 0x17,
                      "legacy final certificate response order");
                CHECK(tx_paylen[2] == 21 && tx_pay[2][20] == 0,
                      "legacy signature request without transaction bytes");
                for (int i=0; i<20; i++) CHECK(tx_pay[2][i] == 0x80 + i,
                                             "legacy challenge byte %d", i);
            }
        }
        fake_us += 1999999;
        iap_tx_pump();
        CHECK(cap_n == 0, "legacy Auth 2.0 fallback waits two seconds");
        fake_us += 1;
        iap_tx_pump();
        CHECK(decode_tx() == 0 && probe_reconnects == 1,
              "legacy auth timeout requests USB-only profile reconnect");
        iap_reset_protocol();
    }

    // 1. IdentifyDeviceLingoes (no trx yet)
    {
        uint8_t c[] = {0x00, 0x13, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x01};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 2, "identify: want 2 tx, got %d", n);
        CHECK(tx_cmds[0] == 0x0002, "identify: first must be ACK");
        CHECK(tx_pay[0][0] == 0x00 && tx_pay[0][1] == 0x13, "identify: ACK success 0x13");
        CHECK(tx_cmds[1] == 0x0014, "identify: second must be GetDevAuthInfo, got %04X", tx_cmds[1]);
    }

    // 2. RetDevAuthenticationInfo v1
    {
        uint8_t c[] = {0x00, 0x15, 0x01, 0x00};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 2, "auth v1: want 2 tx, got %d", n);
        CHECK(tx_cmds[0] == 0x0016 && tx_pay[0][0] == 0x00, "auth v1: AckDevAuthInfo supported");
        CHECK(tx_cmds[1] == 0x0A0002, "auth v1: GetAccSampleRateCaps, got %05X", tx_cmds[1]);
    }

    // 3. RetAccSampleRateCaps -> TrackNewAudioAttributes 48000
    {
        uint8_t c[] = {0x0A, 0x03, 0x00, 0x00, 0xAC, 0x44, 0x00, 0x00, 0xBB, 0x80};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x0A0004, "audio attrs cmd");
        uint32_t sr = ((uint32_t) tx_pay[0][0] << 24) | (tx_pay[0][1] << 16) |
                      (tx_pay[0][2] << 8) | tx_pay[0][3];
        CHECK(sr == 44100, "sample rate 44100, got %u", sr);
    }

    // 4. StartIDPS -> trx on
    {
        uint8_t c[] = {0x00, 0x38};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x0002, "startIDPS ack");
    }

    // 5. SetFIDTokenValues (trx=1): identify + acccaps tokens
    {
        uint8_t c[] = {
            0x00, 0x39, 0x00, 0x01, 0x02,
            0x0E, 0x00, 0x00, 0x03, 0x00, 0x04, 0x0A,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x0A, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
        };
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x003A, "fid ack cmd, got %d/%04X", n, n ? tx_cmds[0] & 0xFFFF : 0);
        if (n == 1) {
            // trx(2) + count + 2 entries of [len,type,sub,ack]
            CHECK(tx_pay[0][2] == 0x02, "fid ack count 2, got %d", tx_pay[0][2]);
            CHECK(tx_paylen[0] == 11, "fid ack len 11, got %d", tx_paylen[0]);
        }
    }

    // 6. EndIDPS continue (trx=2) -> IDPSStatus OK + GetDevAuthInfo
    {
        uint8_t c[] = {0x00, 0x3B, 0x00, 0x02, 0x00};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 2, "endidps: want 2 tx, got %d", n);
        CHECK(tx_cmds[0] == 0x003C && tx_pay[0][2] == 0x00, "IDPSStatus OK");
        CHECK(tx_cmds[1] == 0x0014, "GetDevAuthInfo after IDPS");
    }

    // 7. ExtRemote GetPlayStatus (trx=3), no PCM yet -> paused, pos 0
    {
        uint8_t c[] = {0x04, 0x00, 0x1C, 0x00, 0x03};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x04001D, "playstatus resp");
        if (n == 1) {
            // payload after cmd: trx(2) + len(4) + pos(4) + state(1)
            CHECK(tx_pay[0][10] == 0x02, "state paused, got %02X", tx_pay[0][10]);
            uint32_t pos = ((uint32_t) tx_pay[0][6] << 24) | (tx_pay[0][7] << 16) |
                           (tx_pay[0][8] << 8) | tx_pay[0][9];
            CHECK(pos == 0, "pos 0, got %u", pos);
        }
    }

    // 8. Live metadata + title/artist queries (long title forces multi-report TX)
    iap_set_track("66samus",
        "Team America World Police Best Of Extended Super Long Edition Mix", "Album");
    {
        uint8_t c[] = {0x04, 0x00, 0x20, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x040021, "title resp");
        if (n == 1)
            CHECK(memcmp(tx_pay[0] + 2, "Team America", 12) == 0, "title text (after trx)");
    }
    {
        uint8_t c[] = {0x04, 0x00, 0x22, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x040023, "artist resp");
        if (n == 1)
            CHECK(memcmp(tx_pay[0] + 2, "66samus", 7) == 0, "artist text");
    }

    // 9. GetiPodOptionsForLingo general -> 0x63DEF73FF
    {
        uint8_t c[] = {0x00, 0x4B, 0x00, 0x06, 0x00};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x004C, "options resp");
        if (n == 1) {
            uint64_t o = 0;
            for (int i = 3; i < 11; i++) o = (o << 8) | tx_pay[0][i];
            CHECK(o == 0x000000063DEF73FFull, "options value %llx", (unsigned long long) o);
        }
    }

    // 10. Unknown general command -> ACK UnknownID
    {
        uint8_t c[] = {0x00, 0x77, 0x00, 0x07};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn);
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x0002, "unknown acked");
        // no transaction on the unparseable request: payload is just {status, cmd}
        if (n == 1) CHECK(tx_paylen[0] == 2 && tx_pay[0][0] == 0x05 && tx_pay[0][1] == 0x77, "unknownID+cmd");
    }

    // 11. Wire-ID strip rule shared with firmware.
    {
        uint8_t a[] = {5, 0, 0x55};
        CHECK(iap_strip_id(a, 3) == 1, "strip [ID][link]");
        uint8_t b[] = {0, 0x55, 0x03};
        CHECK(iap_strip_id(b, 3) == 0, "keep link-first");
        uint8_t c[] = {8, 0, 0x55, 1, 2, 3, 4, 5};
        CHECK(iap_strip_id(c, 8) == 1, "strip ID 8");
        // ID-prefixed RequestiPodName through the real path
        uint8_t q[] = {0x00, 0x07, 0x00, 0x09};  // trx=9
        pkt_begin(); pkt_cmd(q, sizeof(q));
        uint8_t wired[16];
        wired[0] = 5; wired[1] = 0;
        memcpy(wired + 2, fbuf, fn);
        int total = fn + 2;
        show_tx("drain2");
        uint16_t sk = iap_strip_id(wired, (uint16_t) total);
        iap_rx_report(5, wired + sk, (uint16_t) (total - sk));
        iap_tx_pump();
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x0008, "id-prefixed frame handled");
        if (n == 1) CHECK(memcmp(tx_pay[0] + 2, "Volvo AirPlay", 13) == 0, "name text");
    }

    // 12. Split RX frame across two reports (MoreToFollow + Continue)
    {
        uint8_t c[] = {0x00, 0x07, 0x00, 0x08};  // RequestiPodName trx=8
        pkt_begin(); pkt_cmd(c, sizeof(c));
        int total = fn, half = total / 2;
        uint8_t r1[8] = {0x02}; memcpy(r1 + 1, fbuf, half);
        uint8_t r2[8] = {0x01}; memcpy(r2 + 1, fbuf + half, total - half);
        show_tx("drain");
        iap_rx_report(5, r1, (uint16_t) (half + 1));
        iap_tx_pump();
        CHECK(cap_n == 0, "no response before frame complete");
        iap_rx_report(5, r2, (uint16_t) (total - half + 1));
        iap_tx_pump();
        int n = decode_tx();
        CHECK(n == 1 && tx_cmds[0] == 0x0008, "split frame reassembled");
        if (n == 1) CHECK(memcmp(tx_pay[0] + 2, "Volvo AirPlay", 13) == 0, "name text");
    }

    // AirPlay input alone must not advance the car's play-position clock.
    iap_set_track("artist", "track", "album");
    iap_set_playing(true);
    iap_note_pcm(176400);
    {
        uint8_t c[] = {4, 0, 0x1c, 0, 0x70};
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn); decode_tx();
        CHECK(tx_pay[0][6] == 0 && tx_pay[0][7] == 0 && tx_pay[0][8] == 0 && tx_pay[0][9] == 0,
              "queued PCM doesn't advance position");
        iap_note_usb_time(1000000);
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn); decode_tx();
        CHECK(tx_pay[0][8] == 3 && tx_pay[0][9] == 0xe8, "completed USB advances position 1000ms");
        iap_set_playing(false);
        iap_note_usb_time(2000000);
        pkt_begin(); pkt_cmd(c, sizeof(c)); feed_frame(fbuf, fn); decode_tx();
        CHECK(tx_pay[0][8] == 3 && tx_pay[0][9] == 0xe8, "paused USB silence doesn't advance position");
    }

    if (failures == 0) printf("\nALL TESTS PASSED\n");
    else printf("\n%d FAILURES\n", failures);
    return failures != 0;
}
