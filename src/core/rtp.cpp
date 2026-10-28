#include "rtp.h"
#include <cstring>

namespace aes67 {

bool rtp_parse(const uint8_t* b, size_t len, RtpHeader& h, const uint8_t** payload, size_t* payload_len) {
    if (len < 12) return false;
    h.version = b[0] >> 6;
    if (h.version != 2) return false;
    h.padding = (b[0] & 0x20) != 0;
    h.extension = (b[0] & 0x10) != 0;
    int cc = b[0] & 0x0f;
    h.marker = (b[1] & 0x80) != 0;
    h.payload_type = b[1] & 0x7f;
    h.sequence = (uint16_t)((b[2] << 8) | b[3]);
    h.timestamp = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    h.ssrc = ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) | ((uint32_t)b[10] << 8) | b[11];
    size_t off = 12 + 4 * (size_t)cc;
    if (off > len) return false;
    if (h.extension) {
        if (off + 4 > len) return false;
        size_t xlen = (((size_t)b[off + 2] << 8) | b[off + 3]) * 4;
        off += 4 + xlen;
        if (off > len) return false;
    }
    size_t end = len;
    if (h.padding) {
        size_t pad = b[len - 1];
        if (pad == 0 || pad > len - off) return false;
        end = len - pad;
    }
    h.header_len = (int)off;
    *payload = b + off;
    *payload_len = end - off;
    return true;
}

void rtp_write_header(uint8_t* b, const RtpHeader& h) {
    b[0] = (uint8_t)(0x80 | (h.padding ? 0x20 : 0) | (h.extension ? 0x10 : 0));
    b[1] = (uint8_t)((h.marker ? 0x80 : 0) | (h.payload_type & 0x7f));
    b[2] = (uint8_t)(h.sequence >> 8); b[3] = (uint8_t)h.sequence;
    b[4] = (uint8_t)(h.timestamp >> 24); b[5] = (uint8_t)(h.timestamp >> 16); b[6] = (uint8_t)(h.timestamp >> 8); b[7] = (uint8_t)h.timestamp;
    b[8] = (uint8_t)(h.ssrc >> 24); b[9] = (uint8_t)(h.ssrc >> 16); b[10] = (uint8_t)(h.ssrc >> 8); b[11] = (uint8_t)h.ssrc;
}

void pcm_unpack(Encoding e, const uint8_t* src, int frames, int stream_ch, int ch_off, int32_t* const* dst, int out_ch) {
    const int bps = encoding_bytes(e);
    const int stride = bps * stream_ch;
    for (int c = 0; c < out_ch; c++) {
        int sc = ch_off + c;
        int32_t* d = dst[c];
        if (sc >= stream_ch) { memset(d, 0, sizeof(int32_t) * frames); continue; }
        const uint8_t* s = src + sc * bps;
        if (bps == 2) {
            for (int i = 0; i < frames; i++, s += stride) d[i] = (int32_t)(((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16));
        } else {
            for (int i = 0; i < frames; i++, s += stride) d[i] = (int32_t)(((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8));
        }
    }
}

void pcm_pack(Encoding e, const int32_t* const* src, int frames, int ch, uint8_t* dst) {
    const int bps = encoding_bytes(e);
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < ch; c++) {
            uint32_t v = (uint32_t)src[c][i];
            if (bps == 2) {
                // round to 16 bits with saturation
                int64_t r = (int64_t)(int32_t)v + 0x8000;
                if (r > 0x7FFFFFFF) r = 0x7FFFFFFF;
                uint32_t u = (uint32_t)r;
                *dst++ = (uint8_t)(u >> 24); *dst++ = (uint8_t)(u >> 16);
            } else {
                *dst++ = (uint8_t)(v >> 24); *dst++ = (uint8_t)(v >> 16); *dst++ = (uint8_t)(v >> 8);
            }
        }
    }
}

} // namespace aes67
