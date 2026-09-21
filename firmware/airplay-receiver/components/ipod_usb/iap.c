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
typedef enum { ST_IDLE, ST_IDENTIFIED, ST_AUTH, ST_AUTH_SIG, ST_AUTH_OK, ST_AUDIO, ST_READY } iap_state_t;
static iap_state_t iap_state;
static int cert_cur = -1, cert_max = -1;
static uint32_t tx_ack, tx_ident, tx_auth, tx_audio, tx_other;
static uint32_t pkt_seq;
static bool stall_dumped;
#define CERT_CAPACITY 8192
static uint8_t certificate[CERT_CAPACITY];
static uint16_t cert_offsets[256], cert_lengths[256], cert_size, cert_next;
static bool audio_requested;

static void reset_handshake(void) {
    iap_state = ST_IDLE;
    cert_cur = cert_max = -1;
    cert_size = cert_next = 0;
    audio_requested = false;
    stall_dumped = false;
}


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
        // HID report count is fixed by the descriptor. Match the reference
        // encoder's zero padding even when the iAP frame itself is shorter.
        uint8_t buf[64] = {0};
        buf[0] = link;
        memcpy(buf + 1, frame + offset, chunk);
        if (!txq_push(def->id, buf, (uint8_t) (def->max_payload + 1))) {
            ipod_usb_log("TX queue full: report=%u remaining=%u", def->id, len);
            break;
        }
        len -= chunk;
        offset += chunk;
    }
    // No direct pump here: the TinyUSB task loop drains the queue. Calling
    // tud_hid_report() from inside the SET_REPORT callback risks stack
    // reentrancy on some ports; ~10 ms extra latency is irrelevant for iAP.
}

static void pump_metadata(void);

void iap_tx_pump(void) {
    pump_metadata();
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
    {0x04, 0x0018, 1}, {0x04, 0x001A, 9}, {0x04, 0x001C, 0},
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
    ipod_usb_log(">> lingo=%02X cmd=%04X trx=%s%04X len=%u lat=%luus",
                 lingo, cmd, has_trx ? "" : "absent/", trx, payload_len,
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
static uint64_t track_start_us, played_us, last_usb_us;
static bool metadata_notify_pending;
static portMUX_TYPE media_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t last_push_us;
static bool playing;
static bool notify_armed;

void iap_set_track(const char *artist, const char *title, const char *album) {
    portENTER_CRITICAL(&media_lock);
    snprintf(np_artist, sizeof(np_artist), "%s", artist ? artist : "");
    snprintf(np_title, sizeof(np_title), "%s", title ? title : "");
    snprintf(np_album, sizeof(np_album), "%s", album ? album : "");
    track_start_us = played_us;
    metadata_notify_pending = true;
    portEXIT_CRITICAL(&media_lock);
}

static void pump_metadata(void) {
    portENTER_CRITICAL(&media_lock);
    bool changed = metadata_notify_pending;
    metadata_notify_pending = false;
    portEXIT_CRITICAL(&media_lock);
    if (changed && notify_armed) {
        uint8_t status = 1;
        tx_notify(LINGO_EXTREM, 0x0027, &status, 1);
    }
}

void iap_set_playing(bool p) {
    portENTER_CRITICAL(&media_lock); playing = p; portEXIT_CRITICAL(&media_lock);
}

bool iap_audio_active(void) {
    portENTER_CRITICAL(&media_lock);
    bool active = playing && (esp_timer_get_time() - last_push_us < 3000000);
    portEXIT_CRITICAL(&media_lock);
    return active;
}

//--------------------------------------------------------------------+
// Handshake state, latency, counters, watchdog
//--------------------------------------------------------------------+

static const char *state_name(iap_state_t s) {
    switch (s) {
    case ST_IDLE: return "IDLE";
    case ST_IDENTIFIED: return "IDENTIFIED";
    case ST_AUTH: return "AUTH_CERT";
    case ST_AUTH_SIG: return "AUTH_SIG";
    case ST_AUTH_OK: return "AUTH_OK";
    case ST_AUDIO: return "AUDIO_CAPS";
    default: return "READY";
    }
}

static const char *expected_next(void) {
    switch (iap_state) {
    case ST_IDLE: return "IdentifyDeviceLingoes/StartIDPS";
    case ST_IDENTIFIED: return "auth sections/LingoVersion";
    case ST_AUTH: return "cert section";
    case ST_AUTH_SIG: return "RetDevAuthenticationSignature";
    case ST_AUTH_OK: return "DigitalAudio start";
    case ST_AUDIO: return "sample rates/AccAck";
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
    ipod_usb_log("*** IAP STALL *** state=%s cert=%d/%d lastlat=%luus expect=%s queued=%u",
                 state_name(iap_state), cert_cur, cert_max,
                 (unsigned long) last_latency_us, expected_next(), txq_count);
}

void iap_note_pcm(uint32_t bytes) {
    (void)bytes;
    portENTER_CRITICAL(&media_lock);
    last_push_us = esp_timer_get_time();
    portEXIT_CRITICAL(&media_lock);
}

void iap_note_usb_time(uint64_t total_us) {
    portENTER_CRITICAL(&media_lock);
    if (playing && esp_timer_get_time() - last_push_us < 3000000)
        played_us += total_us - last_usb_us;
    last_usb_us = total_us;
    portEXIT_CRITICAL(&media_lock);
}

// ms of audio delivered for the current track (44100 Hz stereo s16)
static uint32_t track_pos_ms(void) {
    portENTER_CRITICAL(&media_lock);
    uint64_t elapsed = played_us - track_start_us;
    portEXIT_CRITICAL(&media_lock);
    return (uint32_t)(elapsed / 1000);
}

static uint8_t play_state(void) {
    return iap_audio_active() ? 0x01 : 0x02;
}

//--------------------------------------------------------------------+
// General lingo handler (mirrors Go HandleGeneral)
//--------------------------------------------------------------------+

static void start_digital_audio(void) {
    if (audio_requested) return;
    audio_requested = true;
    iap_state = ST_AUDIO;
    tx_notify(LINGO_AUDIO, 0x02, NULL, 0);
}

static void handle_general(const rx_cmd_t *c) {
    cmd_buf_t r = { .n = 0 };
    uint8_t resp_cmd = 0;
    bool respond = true;

    switch (c->cmd) {
    case 0x00:  // RequestIdentify: no reply in reference flow
        reset_handshake();
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
        reset_handshake();
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
        if (c->len < 2 || (c->p[0] >= 2 && c->len < 4)) goto bad_certificate;
        if (c->p[0] < 2) {
            // Reference v1 flow has no v2 challenge stage.
            uint8_t supported = 0;
            tx_respond(c, LINGO_GENERAL, 0x16, &supported, 1);
            iap_state = ST_AUTH_OK;
            start_digital_audio();
            return;
        }
        uint8_t cur = c->p[2], last = c->p[3];
        uint16_t size = c->len - 4;
        if (cur > last || (cert_max >= 0 && last != cert_max)) goto bad_certificate;
        if (cur < cert_next) {
            // Retransmission: ACK without appending or restarting audio/auth.
            if (size != cert_lengths[cur] ||
                memcmp(certificate + cert_offsets[cur], c->p + 4, size)) goto bad_certificate;
        } else {
            if (cur != cert_next || cert_size + size > CERT_CAPACITY) goto bad_certificate;
            cert_offsets[cur] = cert_size;
            cert_lengths[cur] = size;
            memcpy(certificate + cert_size, c->p + 4, size);
            cert_size += size;
            cert_next++;
            cert_cur = cur; cert_max = last;
            iap_state = ST_AUTH;
        }
        if (cur < last) {
            uint8_t ack[] = {ACK_OK, 0x15};
            tx_respond(c, LINGO_GENERAL, 0x02, ack, sizeof(ack));
        } else {
            uint8_t supported = 0;
            tx_respond(c, LINGO_GENERAL, 0x16, &supported, 1);
            if (iap_state == ST_AUTH) {
                // V2 layout: 20 challenge bytes followed by a retry counter.
                // Rockbox upstream and the digital-audio fork use counter 1;
                // the Go reference uses 0. Test that difference in isolation.
                // Challenge remains deterministic: this emulator does not
                // cryptographically verify the accessory's signature.
                // Respond preserves the final certificate's transaction ID.
                const uint8_t challenge[21] = {[20] = 1};
                ipod_usb_log("auth signature request: v2 challenge=zero20 counter=1");
                tx_respond(c, LINGO_GENERAL, 0x17, challenge, sizeof(challenge));
                iap_state = ST_AUTH_SIG;
                ipod_usb_log("cert complete sections=%u bytes=%u; await signature", cert_next, cert_size);
            }
        }
        return;
    bad_certificate: {
        uint8_t ack[] = {ACK_FAILED, 0x15};
        ipod_usb_log("reject cert len=%u next=%u last=%d", c->len, cert_next, cert_max);
        tx_respond(c, LINGO_GENERAL, 0x02, ack, sizeof(ack));
        return;
    }
    }
    case 0x18: {  // RetDevAuthenticationSignature
        if (c->len == 0 || (iap_state != ST_AUTH_SIG && !audio_requested)) {
            uint8_t ack[] = {ACK_FAILED, 0x18};
            tx_respond(c, LINGO_GENERAL, 0x02, ack, sizeof(ack));
            return;
        }
        // Match reference acceptance; no certificate/signature verification.
        uint8_t passed = 0;
        tx_respond(c, LINGO_GENERAL, 0x19, &passed, 1);
        if (!audio_requested) { iap_state = ST_AUTH_OK; start_digital_audio(); }
        return;
    }
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
        reset_handshake();
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
    if (!audio_requested) {
        ipod_usb_log("ignore DigitalAudio before auth cmd=%02x", c->cmd);
        return;
    }
    if (c->cmd == 0x03) {  // RetAccSampleRateCaps -> announce default rate
        cmd_buf_t r = { .n = 0 };
        put_u32(&r, 44100);
        put_u32(&r, 0);
        put_u32(&r, 0);
        tx_respond(c, LINGO_AUDIO, 0x04, r.b, r.n);
    } else if (c->cmd == 0x00 && c->len >= 2) {
        // AccAck{status, cmdID}: the car's verdict on our last audio command.
        ipod_usb_log("AccAck status=%u cmd=0x%02x", c->p[0], c->p[1]);
        if (c->p[0] == ACK_OK && c->p[1] == 0x04) iap_state = ST_READY;
    }
    // iPodAck and the rest: nothing to do.
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
        put_u32(&r, c->p[0] == 1 || c->p[0] == 2 || c->p[0] == 3 || c->p[0] == 5 ? 1 : 0);
        break;
    case 0x001A: { // category, offset, count (variable-length reference command)
        if (c->len != 9 || c->p[1] || c->p[2] || c->p[3] || c->p[4]) {
            xr_ack(c, ACK_FAILED); return;
        }
        if (!(c->p[5] || c->p[6] || c->p[7] || c->p[8])) return;
        const char *name = NULL;
        portENTER_CRITICAL(&media_lock);
        switch (c->p[0]) {
        case 1: name = "AirPlay"; break;
        case 2: name = np_artist; break;
        case 3: name = np_album; break;
        case 5: name = np_title[0] ? np_title : "AirPlay"; break;
        }
        resp = 0x001B;
        put_u32(&r, 0);
        // Reference record has a fixed 16-byte name field.
        for (unsigned i = 0; i < 16; i++) put_u8(&r, name && i < strlen(name) ? name[i] : 0);
        portEXIT_CRITICAL(&media_lock);
        if (!name) { xr_ack(c, ACK_FAILED); return; }
        break;
    }
    case 0x001C:  // GetPlayStatus: LIVE
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
        portENTER_CRITICAL(&media_lock);
        put_cstr(&r, np_title);
        portEXIT_CRITICAL(&media_lock);
        break;
    case 0x0022:
        resp = 0x0023;
        portENTER_CRITICAL(&media_lock);
        put_cstr(&r, np_artist);
        portEXIT_CRITICAL(&media_lock);
        break;
    case 0x0024:
        resp = 0x0025;
        portENTER_CRITICAL(&media_lock);
        put_cstr(&r, np_album);
        portEXIT_CRITICAL(&media_lock);
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
        ipod_usb_log("unsupported ExtendedRemote cmd=%04x len=%u", c->cmd, c->len);
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
        ipod_usb_log("<< lingo=%02X cmd=%04X trx=%s%04X len=%u",
                     c.lingo, c.cmd, c.has_trx ? "" : "absent/", c.trx, c.len);
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

// Called by USB owner when a bus session ends; retain media and lifetime counters.
void iap_reset_protocol(void) {
    reset_handshake();
    txq_head = txq_tail = txq_count = 0;
    trx_enabled = false; trx_counter = 0;
    frame_len = 0; last_rx_us = 0; notify_armed = false;
}
