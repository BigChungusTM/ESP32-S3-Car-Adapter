// iPod Accessory Protocol engine.
// Port of research/ipod (oandrew/ipod, MIT License (c) 2021 Andrew Onyshchuk):
// packet framing, command serde with transaction tracking, and General +
// DigitalAudio + ExtendedRemote handlers with live now-playing data.
// SimpleRemote/DisplayRemote get logged ACKs.
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

#include "iap.h"
#include "ipod_usb.h"

static const char *TAG = "IAP";

//--------------------------------------------------------------------+
// Lingo / command IDs
//--------------------------------------------------------------------+

#define LINGO_GENERAL   0x00
#define LINGO_SIMPLEREM 0x02
#define LINGO_DISPREM   0x03
#define LINGO_EXTREM    0x04
#define LINGO_AUDIO     0x0A

#define ACK_OK        0x00
#define ACK_FAILED    0x02
#define ACK_UNKNOWNID 0x05

//--------------------------------------------------------------------+
// HID report fragmentation (device->host, AccIn report defs)
//--------------------------------------------------------------------+

typedef struct { uint8_t id; uint8_t max_payload; } report_def_t;
static const report_def_t tx_defs[] = {
    {1, 11}, {2, 13}, {3, 19}, {4, 62},
};

#define LC_DONE 0x00
#define LC_CONT 0x01
#define LC_MORE 0x02

//--------------------------------------------------------------------+
// TX report queue (single-producer task: TinyUSB task)
//--------------------------------------------------------------------+

#define TXQ_N 24
typedef struct { uint8_t id; uint8_t len; uint8_t data[64]; } tx_report_t;
static tx_report_t txq[TXQ_N];
static uint8_t txq_head, txq_tail, txq_count;
static uint32_t tx_packets;
static int64_t last_rx_us;
static uint32_t last_latency_us;
typedef enum { ST_IDLE, ST_IDENTIFIED, ST_AUTH, ST_AUTH_OK, ST_READY } iap_state_t;
static iap_state_t iap_state;
static int cert_cur = -1, cert_max = -1;
static uint32_t tx_ack, tx_ident, tx_auth, tx_audio, tx_other;
static uint32_t pkt_seq;
static bool stall_dumped;

static bool txq_push(uint8_t id, const uint8_t *data, uint8_t len) {
    if (txq_count >= TXQ_N || len > 64) return false;
    tx_report_t *r = &txq[txq_tail];
    r->id = id; r->len = len;
    memcpy(r->data, data, len);
    txq_tail = (uint8_t) ((txq_tail + 1) % TXQ_N);
    txq_count++;
    return true;
}

// Split one iAP frame into HID reports, Go Encoder.Pick semantics.
static void tx_frame(const uint8_t *frame, uint16_t len) {
    uint16_t offset = 0;
    while (len > 0) {
        const report_def_t *def = &tx_defs[3];
        for (unsigned i = 0; i < 4; i++) {
            if (tx_defs[i].max_payload >= len) { def = &tx_defs[i]; break; }
        }
        uint16_t chunk = len > def->max_payload ? def->max_payload : len;
        uint8_t link = LC_DONE;
        if (len > def->max_payload) link = (offset == 0) ? LC_MORE : (LC_CONT | LC_MORE);
        else if (offset > 0) link = LC_CONT;
        uint8_t buf[64];
        buf[0] = link;
        memcpy(buf + 1, frame + offset, chunk);
        if (!txq_push(def->id, buf, (uint8_t) (chunk + 1))) break;
        len -= chunk;
        offset += chunk;
    }
    // No direct pump here: the TinyUSB task loop drains the queue. Calling
    // tud_hid_report() from inside the SET_REPORT callback risks stack
    // reentrancy on some ports; ~10 ms extra latency is irrelevant for iAP.
}

void iap_tx_pump(void) {
    while (txq_count > 0) {
        tx_report_t *r = &txq[txq_head];
        if (!tud_hid_report(r->id, r->data, r->len)) break;
        txq_head = (uint8_t) ((txq_head + 1) % TXQ_N);
        txq_count--;
        tx_packets++;
    }
}

bool iap_tx_pending(void) { return txq_count > 0; }

//--------------------------------------------------------------------+
// Packet framing (0x55, evidente checksum: bytes sum to 0 mod 256)
//--------------------------------------------------------------------+

static uint8_t cksum(const uint8_t *p, uint16_t n) {
    uint8_t s = 0;
    for (uint16_t i = 0; i < n; i++) s += p[i];
    return (uint8_t) -s;
}

// Encode one command payload into a full packet. Returns packet length.
static uint16_t pkt_encode(const uint8_t *cmd, uint16_t cmd_len, uint8_t *out) {
    uint16_t o = 0;
    out[o++] = 0x55;
    if (cmd_len > 256) {
        out[o++] = 0x00;
        out[o++] = (uint8_t) (cmd_len >> 8);
        out[o++] = (uint8_t) cmd_len;
    } else {
        out[o++] = (uint8_t) cmd_len;
    }
    memcpy(out + o, cmd, cmd_len);
    o += cmd_len;
    uint8_t c = cksum(out + 1, o - 1);
    out[o++] = c;
    return o;
}

//--------------------------------------------------------------------+
// Command encode helpers (big-endian)
//--------------------------------------------------------------------+

#define CMD_BUF 512
typedef struct {
    uint8_t b[CMD_BUF];
    uint16_t n;
} cmd_buf_t;

static void put_u8(cmd_buf_t *c, uint8_t v) { if (c->n < CMD_BUF) c->b[c->n++] = v; }
static void put_u16(cmd_buf_t *c, uint16_t v) { put_u8(c, (uint8_t) (v >> 8)); put_u8(c, (uint8_t) v); }
static void put_u32(cmd_buf_t *c, uint32_t v) { put_u16(c, (uint16_t) (v >> 16)); put_u16(c, (uint16_t) v); }
static void put_u64(cmd_buf_t *c, uint64_t v) { put_u32(c, (uint32_t) (v >> 32)); put_u32(c, (uint32_t) v); }
static void put_bytes(cmd_buf_t *c, const uint8_t *p, uint16_t n) {
    for (uint16_t i = 0; i < n && c->n < CMD_BUF; i++) c->b[c->n++] = p[i];
}
static void put_cstr(cmd_buf_t *c, const char *s) {
    while (*s && c->n < CMD_BUF) c->b[c->n++] = (uint8_t) *s++;
    if (c->n < CMD_BUF) c->b[c->n++] = 0;
}

//--------------------------------------------------------------------+
// Transaction tracking (mirrors Go handleCmdID)
//--------------------------------------------------------------------+

static bool trx_enabled;
static uint32_t trx_counter;

static void trx_track(uint8_t lingo, uint16_t cmd) {
    if (lingo != LINGO_GENERAL) return;
    bool prev = trx_enabled;
    if (cmd == 0x00 || cmd == 0x13) trx_enabled = false;
    else if (cmd == 0x38) trx_enabled = true;
    if (prev != trx_enabled) {
        trx_counter = 0;
        ESP_LOGI(TAG, "transactions %s", trx_enabled ? "on" : "off");
    }
}

// Parsed inbound command.
typedef struct {
    uint8_t lingo;
    uint16_t cmd;
    bool has_trx;
    uint16_t trx;
    const uint8_t *p;   // payload after cmd[/trx]
    uint16_t len;
} rx_cmd_t;

// Fixed RX payload sizes per (lingo, cmd); -1 = variable (use trx_enabled).
// trx, when present, adds 2 bytes before the payload.
typedef struct { uint8_t lingo; uint16_t cmd; int8_t size; } cmd_size_t;
static const cmd_size_t cmd_sizes[] = {
    {0x00, 0x00, 0}, {0x00, 0x02, 2}, {0x00, 0x03, 0}, {0x00, 0x05, 0},
    {0x00, 0x06, 0}, {0x00, 0x07, 0}, {0x00, 0x09, 0}, {0x00, 0x0B, 0},
    {0x00, 0x0D, 0}, {0x00, 0x0F, 1}, {0x00, 0x11, 0}, {0x00, 0x13, 12},
    {0x00, 0x15, -1}, {0x00, 0x18, -1}, {0x00, 0x1C, 1}, {0x00, 0x1D, 21},
    {0x00, 0x1F, 1}, {0x00, 0x24, 0}, {0x00, 0x28, -1}, {0x00, 0x29, 1},
    {0x00, 0x2B, 3}, {0x00, 0x35, 0}, {0x00, 0x37, 1}, {0x00, 0x38, 0},
    {0x00, 0x39, -1}, {0x00, 0x3B, 1}, {0x00, 0x46, 4}, {0x00, 0x48, -1},
    {0x00, 0x49, 8}, {0x00, 0x4B, 1}, {0x00, 0x4D, 0},     {0x00, 0x50, 5},
    {0x00, 0x54, 2}, {0x00, 0x64, -1}, {0x00, 0x65, 0},
    {0x00, 0x41, 1}, {0x00, 0x42, -1}, {0x00, 0x43, -1},
    {0x04, 0x0001, 2},
    {0x0A, 0x00, 2}, {0x0A, 0x01, 2}, {0x0A, 0x03, -1},
    {0x04, 0x0004, 0}, {0x04, 0x0007, 4}, {0x04, 0x0009, 0},
    {0x04, 0x000B, 1}, {0x04, 0x000C, 7}, {0x04, 0x000E, 0},
    {0x04, 0x0010, 6}, {0x04, 0x0016, 0}, {0x04, 0x0017, 5},
    {0x04, 0x0018, 1}, {0x04, 0x001A, 5}, {0x04, 0x001C, 0},
    {0x04, 0x001E, 0}, {0x04, 0x0020, 4}, {0x04, 0x0022, 4},
    {0x04, 0x0024, 4}, {0x04, 0x0026, 4}, {0x04, 0x0028, 4},
    {0x04, 0x0029, 1}, {0x04, 0x002A, 8}, {0x04, 0x002C, 0},
    {0x04, 0x002E, 1}, {0x04, 0x002F, 0}, {0x04, 0x0031, 1},
    {0x04, 0x0032, -1}, {0x04, 0x0033, 0}, {0x04, 0x0035, 0},
    {0x04, 0x0037, 4}, {0x04, 0x0038, 5}, {0x04, 0x0039, 0},
    {0x04, 0x003B, 1}, {0x04, 0x003C, 0}, {0x04, 0x003E, 0},
    {0x04, 0x0040, 0}, {0x04, 0x0042, 0},
    {0x02, 0x00, 1},  // simple remote button status (best effort)
};

static bool parse_cmd(const uint8_t *pkt, uint16_t pkt_len, rx_cmd_t *c) {
    if (pkt_len < 2) return false;
    c->lingo = pkt[0];
    uint16_t o = 1;
    if (c->lingo == LINGO_EXTREM) {
        if (pkt_len < 3) return false;
        c->cmd = (uint16_t) ((pkt[1] << 8) | pkt[2]);
        o = 3;
    } else {
        c->cmd = pkt[1];
        o = 2;
    }
    uint16_t rest = pkt_len - o;
    int8_t want = -2;
    for (unsigned i = 0; i < sizeof(cmd_sizes) / sizeof(cmd_sizes[0]); i++) {
        if (cmd_sizes[i].lingo == c->lingo && cmd_sizes[i].cmd == c->cmd) {
            want = cmd_sizes[i].size;
            break;
        }
    }
    c->has_trx = false;
    c->trx = 0;
    if (want >= 0) {
        if (rest == (uint16_t) (want + 2)) {
            c->has_trx = true;
            c->trx = (uint16_t) ((pkt[o] << 8) | pkt[o + 1]);
            o += 2;
        } else if (rest != (uint16_t) want) {
            return false;
        }
    } else if (want == -1) {
        if (trx_enabled && rest >= 2) {
            // Variable payload: assume leading transaction when enabled.
            // Disambiguated by handlers that know exact layouts.
            c->has_trx = true;
            c->trx = (uint16_t) ((pkt[o] << 8) | pkt[o + 1]);
            o += 2;
        }
    } else {
        return false;  // unknown command
    }
    c->p = pkt + o;
    c->len = pkt_len - o;
    trx_track(c->lingo, c->cmd);
    return true;
}

//--------------------------------------------------------------------+
// TX command builders
//--------------------------------------------------------------------+

static uint8_t tx_pkt[600];

static void tx_send(uint8_t lingo, uint16_t cmd, bool use_trx, uint16_t trx,
                    const uint8_t *payload, uint16_t payload_len) {
    cmd_buf_t c = { .n = 0 };
    put_u8(&c, lingo);
    if (lingo == LINGO_EXTREM) put_u16(&c, cmd);
    else put_u8(&c, (uint8_t) cmd);
    bool has_trx = trx_enabled && use_trx;
    if (has_trx) put_u16(&c, trx);
    put_bytes(&c, payload, payload_len);
    trx_track(lingo, cmd);
    last_latency_us = (uint32_t) (esp_timer_get_time() - last_rx_us);
    if ((lingo == LINGO_GENERAL && cmd == 0x02) ||
        (lingo == LINGO_EXTREM && cmd == 0x0001)) tx_ack++;
    else if (lingo == LINGO_GENERAL && (cmd == 0x08 || cmd == 0x0A || cmd == 0x0C ||
             cmd == 0x0E || cmd == 0x10 || cmd == 0x12)) tx_ident++;
    else if (lingo == LINGO_GENERAL) tx_auth++;
    else if (lingo == LINGO_AUDIO) tx_audio++;
    else tx_other++;
    ipod_usb_log(">> lingo=%02X cmd=%04X trx=%s len=%u lat=%luus",
                 lingo, cmd, has_trx ? "y" : "n", payload_len,
                 (unsigned long) last_latency_us);
    uint16_t n = pkt_encode(c.b, c.n, tx_pkt);
    tx_frame(tx_pkt, n);
}

static uint16_t next_trx(void) {
    trx_counter++;
    return (uint16_t) trx_counter;
}

// Respond mirrors the request transaction; Send allocates a fresh one.
static void tx_respond(const rx_cmd_t *req, uint8_t lingo, uint16_t cmd,
                       const uint8_t *payload, uint16_t len) {
    tx_send(lingo, cmd, req->has_trx, req->trx, payload, len);
}

static void tx_notify(uint8_t lingo, uint16_t cmd, const uint8_t *payload, uint16_t len) {
    tx_send(lingo, cmd, true, next_trx(), payload, len);
}

//--------------------------------------------------------------------+
// Live now-playing state (fed from the AirPlay side)
//--------------------------------------------------------------------+

static char np_artist[192], np_title[192], np_album[192];
static char dev_serial[32];
static uint64_t pcm_total_bytes;
static uint64_t track_start_bytes;
static int64_t last_push_us;
static bool playing;
static bool notify_armed;

void iap_set_track(const char *artist, const char *title, const char *album) {
    snprintf(np_artist, sizeof(np_artist), "%s", artist ? artist : "");
    snprintf(np_title, sizeof(np_title), "%s", title ? title : "");
    snprintf(np_album, sizeof(np_album), "%s", album ? album : "");
    track_start_bytes = pcm_total_bytes;
    if (notify_armed) {
        uint8_t st = 0x01;  // track changed
        tx_notify(LINGO_EXTREM, 0x0027, &st, 1);
    }
}

void iap_set_playing(bool p) { playing = p; }

bool iap_audio_active(void) {
    return playing && (esp_timer_get_time() - last_push_us < 3000000);
}

//--------------------------------------------------------------------+
// Handshake state, latency, counters, watchdog
//--------------------------------------------------------------------+

static const char *state_name(iap_state_t s) {
    switch (s) {
    case ST_IDLE: return "IDLE";
    case ST_IDENTIFIED: return "IDENTIFIED";
    case ST_AUTH: return "AUTH_CERT";
    case ST_AUTH_OK: return "AUTH_OK";
    default: return "READY";
    }
}

static const char *expected_next(void) {
    switch (iap_state) {
    case ST_IDLE: return "IdentifyDeviceLingoes/StartIDPS";
    case ST_IDENTIFIED: return "auth sections/LingoVersion";
    case ST_AUTH: return "cert section";
    case ST_AUTH_OK: return "player queries";
    default: return "player queries";
    }
}

void iap_snapshot(iap_snapshot_t *out) {
    snprintf(out->state, sizeof(out->state), "%s", state_name(iap_state));
    out->cert_cur = cert_cur;
    out->cert_max = cert_max;
    out->last_latency_us = last_latency_us;
    out->tx_ack = tx_ack;
    out->tx_ident = tx_ident;
    out->tx_auth = tx_auth;
    out->tx_audio = tx_audio;
    out->tx_other = tx_other;
    out->seq = pkt_seq;
}

void iap_watchdog(void) {
    if (iap_state == ST_IDLE || iap_state == ST_READY || stall_dumped) return;
    if (last_rx_us == 0) return;
    if (esp_timer_get_time() - last_rx_us < 2000000) return;
    stall_dumped = true;
    ipod_usb_log("*** IAP STALL *** state=%s cert=%d/%d lastlat=%luus expect=%s",
                 state_name(iap_state), cert_cur, cert_max,
                 (unsigned long) last_latency_us, expected_next());
}

void iap_note_pcm(uint32_t bytes) {
    pcm_total_bytes += bytes;
    last_push_us = esp_timer_get_time();
}

// ms of audio delivered for the current track (44100 Hz stereo s16)
static uint32_t track_pos_ms(void) {
    uint64_t b = pcm_total_bytes - track_start_bytes;
    return (uint32_t) ((b * 1000) / 176400);
}

static uint8_t play_state(void) {
    if (!playing) return 0x02;  // paused
    if (esp_timer_get_time() - last_push_us > 3000000) return 0x02;
    return 0x01;  // playing
}

//--------------------------------------------------------------------+
// General lingo handler (mirrors Go HandleGeneral)
//--------------------------------------------------------------------+

static void handle_general(const rx_cmd_t *c) {
    cmd_buf_t r = { .n = 0 };
    uint8_t resp_cmd = 0;
    bool respond = true;

    switch (c->cmd) {
    case 0x00:  // RequestIdentify: no reply in reference flow
        respond = false;
        break;
    case 0x02:  // ACK to one of our sends: logged, nothing to do
        respond = false;
        break;
    case 0x03:  // RequestRemoteUIMode
        resp_cmd = 0x04;
        put_u8(&r, 0x00);  // standard mode
        break;
    case 0x05:  // EnterRemoteUIMode
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x05);
        break;
    case 0x06:  // ExitRemoteUIMode
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x06);
        break;
    case 0x07:
        resp_cmd = 0x08;
        put_cstr(&r, "Volvo AirPlay");
        break;
    case 0x09:
        resp_cmd = 0x0A;
        put_u8(&r, 7); put_u8(&r, 1); put_u8(&r, 2);
        break;
    case 0x0B:
        resp_cmd = 0x0C;
        put_cstr(&r, dev_serial);
        break;
    case 0x0D:
        resp_cmd = 0x0E;
        put_u32(&r, 0x00111349);
        put_cstr(&r, "MC676");
        break;
    case 0x0F: {  // RequestLingoProtocolVersion
        uint8_t lingo = c->len >= 1 ? c->p[0] : 0x00;
        resp_cmd = 0x10;
        put_u8(&r, lingo);
        switch (lingo) {
        case LINGO_GENERAL: put_u8(&r, 1); put_u8(&r, 9); break;
        case LINGO_DISPREM: put_u8(&r, 1); put_u8(&r, 5); break;
        case LINGO_EXTREM: put_u8(&r, 1); put_u8(&r, 12); break;
        case LINGO_AUDIO: put_u8(&r, 1); put_u8(&r, 2); break;
        default: put_u8(&r, 1); put_u8(&r, 1); break;
        }
        break;
    }
    case 0x11:
        resp_cmd = 0x12;
        put_u16(&r, 65535);
        break;
    case 0x13: {  // IdentifyDeviceLingoes
        iap_state = ST_IDENTIFIED;
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x13);
        tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
        if (c->len >= 12) {
            uint32_t dev_id = ((uint32_t) c->p[8] << 24) | ((uint32_t) c->p[9] << 16) |
                              ((uint32_t) c->p[10] << 8) | c->p[11];
            if (dev_id != 0) {
                tx_notify(LINGO_GENERAL, 0x14, NULL, 0);  // GetDevAuthenticationInfo
            }
        }
        return;
    }
    case 0x15: {  // RetDevAuthenticationInfo
        iap_state = ST_AUTH;
        if (c->len >= 2 && c->p[0] >= 2) {
            if (c->len >= 4) { cert_cur = c->p[2]; cert_max = c->p[3]; }
            uint8_t cur = c->len >= 3 ? c->p[2] : 0;
            uint8_t max = c->len >= 4 ? c->p[3] : 0;
            if (cur < max) {
                resp_cmd = 0x02;
                put_u8(&r, ACK_OK); put_u8(&r, 0x15);
                tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
            } else {
                resp_cmd = 0x16;
                put_u8(&r, 0x00);  // supported
                tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
                uint8_t sig[21];
                memset(sig, 0, sizeof(sig));  // challenge zeros, counter 0
                tx_notify(LINGO_GENERAL, 0x17, sig, sizeof(sig));
            }
        } else {
            resp_cmd = 0x16;
            put_u8(&r, 0x00);
            tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
        }
        // Audio handshake follows auth completion (mirrors Go handlePacket).
        tx_notify(LINGO_AUDIO, 0x02, NULL, 0);  // GetAccSampleRateCaps
        return;
    }
    case 0x18:  // RetDevAuthenticationSignature
        iap_state = ST_AUTH_OK;
        resp_cmd = 0x19;
        put_u8(&r, 0x00);  // passed
        break;
    case 0x1A:
        resp_cmd = 0x1B;
        put_u8(&r, 1); put_u8(&r, 1); put_u8(&r, 0); put_u8(&r, 0);
        break;
    case 0x1C:  // AckiPodAuthenticationInfo: ignore
        respond = false;
        break;
    case 0x1D:  // GetiPodAuthenticationSignature: echo challenge
        resp_cmd = 0x1E;
        put_bytes(&r, c->p, c->len >= 20 ? 20 : c->len);
        break;
    case 0x1F:  // AckiPodAuthenticationStatus: ignore
        respond = false;
        break;
    case 0x24:
        resp_cmd = 0x25;
        put_u64(&r, 0);
        break;
    case 0x28:  // RetAccessoryInfo: ignore
        respond = false;
        break;
    case 0x29:
        resp_cmd = 0x2A;
        put_u8(&r, c->len >= 1 ? c->p[0] : 0);
        put_u8(&r, 0);
        break;
    case 0x2B:
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x2B);
        break;
    case 0x35:
        resp_cmd = 0x36;
        put_u8(&r, 0x00);
        break;
    case 0x37:  // SetUIMode: accept
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x37);
        break;
    case 0x38:  // StartIDPS
        iap_state = ST_IDENTIFIED;
        trx_counter = 0;
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x38);
        break;
    case 0x39: {  // SetFIDTokenValues: ACK each token {0x00[, type/idx]}
        resp_cmd = 0x3A;
        if (c->len < 1) { respond = false; break; }
        uint8_t count = c->p[0];
        put_u8(&r, count);
        uint16_t o = 1;
        for (uint8_t i = 0; i < count && o + 2 <= c->len; i++) {
            uint8_t tlen = c->p[o++];
            if (o + tlen > c->len) break;
            const uint8_t *t = c->p + o;
            o += tlen;
            if (tlen < 2) continue;
            put_u8(&r, (uint8_t) (2 + 2));  // ack entry length placeholder? (fixed below)
            // Rebuild properly: [len][fidType,fidSubtype][ack...]
            r.n--;  // drop placeholder, write exact below
            uint8_t ftype = t[0], fsub = t[1];
            uint8_t ack_body[4];
            uint8_t ack_len = 0;
            if (ftype == 0x00 && fsub == 0x02) {          // acc info: {0x00, type}
                ack_body[0] = 0x00; ack_body[1] = tlen >= 3 ? t[2] : 0; ack_len = 2;
            } else if (ftype == 0x00 && fsub == 0x03) {   // ipod pref: {0x00, class}
                ack_body[0] = 0x00; ack_body[1] = tlen >= 3 ? t[2] : 0; ack_len = 2;
            } else if (ftype == 0x00 && fsub == 0x04) {   // EA protocol: {0x00, idx}
                ack_body[0] = 0x00; ack_body[1] = tlen >= 3 ? t[2] : 0; ack_len = 2;
            } else {                                      // {0x00}
                ack_body[0] = 0x00; ack_len = 1;
            }
            put_u8(&r, (uint8_t) (2 + ack_len));
            put_u8(&r, ftype); put_u8(&r, fsub);
            put_bytes(&r, ack_body, ack_len);
        }
        break;
    }
    case 0x3B: {  // EndIDPS
        uint8_t st = c->len >= 1 ? c->p[0] : 0x02;
        if (st == 0x00) {
            resp_cmd = 0x3C;
            put_u8(&r, 0x00);  // OK
            tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
            tx_notify(LINGO_GENERAL, 0x14, NULL, 0);  // GetDevAuthenticationInfo
        } else if (st == 0x01) {
            resp_cmd = 0x3C;
            put_u8(&r, 0x04);
        } else if (st == 0x02) {
            resp_cmd = 0x3C;
            put_u8(&r, 0x06);
        } else {
            respond = false;
        }
        if (st != 0x00) break;
        return;
    }
    case 0x46:  // SetAccStatusNotification: ignore
    case 0x48:  // AccessoryStatusNotification: ignore
        respond = false;
        break;
    case 0x49:  // SetEventNotification: accept
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x49);
        break;
    case 0x4B: {  // GetiPodOptionsForLingo
        uint8_t lingo = c->len >= 1 ? c->p[0] : 0x00;
        resp_cmd = 0x4C;
        put_u8(&r, lingo);
        put_u64(&r, lingo == LINGO_GENERAL ? 0x000000063DEF73FFULL : 0);
        break;
    }
    case 0x4D:
        resp_cmd = 0x4E;
        put_u64(&r, 0);
        break;
    case 0x50:  // CancelCommand
        resp_cmd = 0x02;
        put_u8(&r, ACK_OK); put_u8(&r, 0x50);
        break;
    case 0x54:  // SetAvailableCurrent: ignore
        respond = false;
        break;
    case 0x41: case 0x42: case 0x43:  // data-session commands: not used
        respond = false;
        break;
    case 0x64:
        resp_cmd = 0x02;
        put_u8(&r, ACK_FAILED); put_u8(&r, 0x64);
        break;
    case 0x65:
        resp_cmd = 0x66;
        put_u8(&r, 0);  // empty app id
        break;
    default:  // unknown: ACK UnknownID
        resp_cmd = 0x02;
        put_u8(&r, ACK_UNKNOWNID); put_u8(&r, (uint8_t) c->cmd);
        break;
    }
    if (respond) tx_respond(c, LINGO_GENERAL, resp_cmd, r.b, r.n);
}

//--------------------------------------------------------------------+
// DigitalAudio lingo handler (mirrors Go HandleAudio)
//--------------------------------------------------------------------+

static void handle_audio(const rx_cmd_t *c) {
    if (c->cmd == 0x03) {  // RetAccSampleRateCaps -> announce default rate
        cmd_buf_t r = { .n = 0 };
        put_u32(&r, 44100);
        put_u32(&r, 0);
        put_u32(&r, 0);
        tx_respond(c, LINGO_AUDIO, 0x04, r.b, r.n);
    }
    // AccAck/iPodAck and the rest: nothing to do.
}

//--------------------------------------------------------------------+
// ExtendedRemote lingo handler (mirrors Go HandleExtRemote, live data)
//--------------------------------------------------------------------+

static void xr_ack(const rx_cmd_t *c, uint8_t status) {
    cmd_buf_t r = { .n = 0 };
    put_u8(&r, status);
    put_u16(&r, c->cmd);
    tx_respond(c, LINGO_EXTREM, 0x0001, r.b, r.n);
}

static void handle_extremote(const rx_cmd_t *c) {
    cmd_buf_t r = { .n = 0 };
    uint16_t resp = 0;
    bool respond = true;

    switch (c->cmd) {
    case 0x0001:  // ACK to one of our sends
        respond = false;
        break;
    case 0x0002:  // chapter info
        resp = 0x0003;
        put_u32(&r, 0); put_u32(&r, 1);
        break;
    case 0x0004: xr_ack(c, ACK_OK); return;
    case 0x0005:
        resp = 0x0006;
        put_u32(&r, 0); put_u32(&r, 0);
        break;
    case 0x0007:
        resp = 0x0008;
        put_cstr(&r, "chapter");
        break;
    case 0x0009:
        resp = 0x000A;
        put_u8(&r, 0);
        break;
    case 0x000B: xr_ack(c, ACK_OK); return;
    case 0x000C: {  // GetIndexedPlayingTrackInfo
        if (c->len < 7) { xr_ack(c, ACK_FAILED); return; }
        uint8_t type = c->p[0];
        resp = 0x000D;
        put_u8(&r, type);
        if (type == 0x00) {  // caps
            put_u32(&r, 0);
            put_u32(&r, 0);  // unknown length
            put_u16(&r, 1);
        } else if (type == 0x03 || type == 0x04) {  // description/lyrics
            put_u8(&r, 0); put_u16(&r, 0); put_u8(&r, 0);
        } else if (type == 0x07) {  // artwork count: empty
        } else {
            put_u8(&r, 0);
        }
        break;
    }
    case 0x000E:  // GetArtworkFormats: none
        resp = 0x000F;
        break;
    case 0x0010: xr_ack(c, ACK_FAILED); return;
    case 0x0016: xr_ack(c, ACK_OK); return;
    case 0x0017: xr_ack(c, ACK_OK); return;
    case 0x0018:
        resp = 0x0019;
        put_u32(&r, 1);
        break;
    case 0x001A:
        resp = 0x001B;
        put_u32(&r, 0);
        break;
    case 0x001C:  // GetPlayStatus: LIVE
        iap_state = ST_READY;
        resp = 0x001D;
        put_u32(&r, 0);  // length unknown
        put_u32(&r, track_pos_ms());
        put_u8(&r, play_state());
        break;
    case 0x001E:
        resp = 0x001F;
        put_u32(&r, 0);
        break;
    case 0x0020:
        resp = 0x0021;
        put_cstr(&r, np_title);
        break;
    case 0x0022:
        resp = 0x0023;
        put_cstr(&r, np_artist);
        break;
    case 0x0024:
        resp = 0x0025;
        put_cstr(&r, np_album);
        break;
    case 0x0026:  // SetPlayStatusChangeNotification (+short form)
        notify_armed = true;
        xr_ack(c, ACK_OK);
        return;
    case 0x0028: xr_ack(c, ACK_OK); return;
    case 0x0029:  // PlayControl: ACK + log (no iPhone reverse channel yet)
        ipod_usb_log("PlayControl cmd=0x%02x", c->len >= 1 ? c->p[0] : 0xFF);
        xr_ack(c, ACK_OK);
        return;
    case 0x002A:  // GetTrackArtworkTimes: empty
        resp = 0x002B;
        break;
    case 0x002C:
        resp = 0x002D;
        put_u8(&r, 0);
        break;
    case 0x002E: xr_ack(c, ACK_OK); return;
    case 0x002F:
        resp = 0x0030;
        put_u8(&r, 0);
        break;
    case 0x0031: xr_ack(c, ACK_OK); return;
    case 0x0033:
        resp = 0x0034;
        put_u16(&r, 640); put_u16(&r, 960); put_u8(&r, 1);
        break;
    case 0x0035:
        resp = 0x0036;
        put_u32(&r, 1);
        break;
    case 0x0039:
        resp = 0x003A;
        put_u16(&r, 640); put_u16(&r, 960); put_u8(&r, 1);
        break;
    case 0x003B: xr_ack(c, ACK_FAILED); return;
    case 0x0032: xr_ack(c, ACK_OK); return;
    case 0x0037:  // SetCurrentPlayingTrack: accept silently
    case 0x0038:
    case 0x003C: case 0x003E: case 0x0040: case 0x0042:
        respond = false;
        break;
    default:
        xr_ack(c, ACK_FAILED);
        return;
    }
    if (respond) tx_respond(c, LINGO_EXTREM, resp, r.b, r.n);
}

//--------------------------------------------------------------------+
// RX report reassembly + frame dispatch
//--------------------------------------------------------------------+

#define FRAME_MAX 2048
static uint8_t frame_buf[FRAME_MAX];
static uint16_t frame_len;
static uint32_t rx_packets;

static void dispatch_frame(const uint8_t *f, uint16_t n);

void iap_rx_report(uint8_t report_id, const uint8_t *data, uint16_t len) {
    (void) report_id;
    if (len < 1 || len > 65) return;
    uint8_t link = data[0];
    const uint8_t *chunk = data + 1;
    uint16_t chunk_len = len - 1;
    bool first = (link == LC_DONE || link == LC_MORE);
    if (first) frame_len = 0;
    if (frame_len + chunk_len > FRAME_MAX) frame_len = 0;
    memcpy(frame_buf + frame_len, chunk, chunk_len);
    frame_len += chunk_len;
    if (!first)
        ipod_usb_log("frag acc=%u", frame_len);
    if (link == LC_DONE || link == LC_CONT) {
        if (frame_len > (uint16_t) (chunk_len + 1))
            ipod_usb_log("frag COMPLETE total=%u", frame_len);
        dispatch_frame(frame_buf, frame_len);
        frame_len = 0;
    }
}

static void dispatch_packets(const uint8_t *f, uint16_t n) {
    uint16_t o = 0;
    while (o < n) {
        // find start byte
        while (o < n && f[o] != 0x55) o++;
        if (o >= n) break;
        o++;
        if (o >= n) break;
        uint16_t pay_len, hdr;
        if (f[o] == 0x00) {
            if (o + 2 >= n) break;
            pay_len = (uint16_t) ((f[o + 1] << 8) | f[o + 2]);
            hdr = 3;
        } else {
            pay_len = f[o];
            hdr = 1;
        }
        if (o + hdr + pay_len + 1 > n) break;  // incomplete: wait for more
        const uint8_t *pkt = f + o;  // len bytes + payload + checksum
        uint8_t s = 0;
        for (uint16_t i = 0; i < hdr + pay_len + 1; i++) s += pkt[i];
        o += hdr + pay_len + 1;
        if (s != 0) {
            ipod_usb_log("bad iAP checksum, skip");
            continue;
        }
        rx_packets++;
        pkt_seq++;
        last_rx_us = esp_timer_get_time();
        stall_dumped = false;
        rx_cmd_t c;
        if (!parse_cmd(pkt + hdr, pay_len, &c)) {
            ipod_usb_log("unparsed cmd lingo=%02X", pkt[hdr]);
            // ACK unknown like the reference
            if (pkt[hdr] == LINGO_GENERAL && pay_len >= 2) {
                cmd_buf_t r = { .n = 0 };
                put_u8(&r, ACK_UNKNOWNID); put_u8(&r, pkt[hdr + 1]);
                rx_cmd_t fake = { .lingo = LINGO_GENERAL, .cmd = pkt[hdr + 1],
                                  .has_trx = false, .trx = 0 };
                tx_respond(&fake, LINGO_GENERAL, 0x02, r.b, r.n);
            }
            continue;
        }
        ipod_usb_log("<< lingo=%02X cmd=%04X trx=%s len=%u",
                     c.lingo, c.cmd, c.has_trx ? "y" : "n", c.len);
        switch (c.lingo) {
        case LINGO_GENERAL: handle_general(&c); break;
        case LINGO_AUDIO: handle_audio(&c); break;
        case LINGO_EXTREM: handle_extremote(&c); break;
        case LINGO_SIMPLEREM:
        case LINGO_DISPREM:
            // Not implemented yet: logged above; extended if the car uses them.
            break;
        default: break;
        }
    }
}

static void dispatch_frame(const uint8_t *f, uint16_t n) {
    dispatch_packets(f, n);
}

uint32_t iap_rx_packets(void) { return rx_packets; }
uint32_t iap_tx_packets(void) { return tx_packets; }

void iap_set_serial(const char *serial) {
    snprintf(dev_serial, sizeof(dev_serial), "%s", serial ? serial : "");
}
