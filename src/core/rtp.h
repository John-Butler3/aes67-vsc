// RTP (RFC 3550) header handling and L16/L24 sample packing.
#pragma once
#include <cstdint>
#include <cstddef>
#include "sdp.h"

namespace aes67 {

struct RtpHeader {
    uint8_t  version = 2;
    bool     padding = false;
    bool     extension = false;
    bool     marker = false;
    uint8_t  payload_type = 98;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    int      header_len = 12; // including CSRC and extension
};

// Parse header; returns false if malformed. payload/payload_len point into buf.
bool rtp_parse(const uint8_t* buf, size_t len, RtpHeader& h, const uint8_t** payload, size_t* payload_len);
// Write a 12-byte header.
void rtp_write_header(uint8_t* buf, const RtpHeader& h);

// Sample conversion. Internal format is int32 with the audio in the top 24 bits (ASIOSTInt32LSB).
// Interleaved network order <-> per-channel int32 arrays.
// dst[ch][frame], src is interleaved big-endian with 'stream_ch' channels; copies channels [ch_off, ch_off+out_ch).
void pcm_unpack(Encoding e, const uint8_t* src, int frames, int stream_ch, int ch_off, int32_t* const* dst, int out_ch);
// Pack per-channel int32 into interleaved big-endian network samples.
void pcm_pack(Encoding e, const int32_t* const* src, int frames, int ch, uint8_t* dst);

// Wrap-safe signed difference a - b for 32-bit RTP timestamps.
inline int32_t ts_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }
inline int16_t seq_diff(uint16_t a, uint16_t b) { return (int16_t)(a - b); }

} // namespace aes67
