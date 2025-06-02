#pragma once

#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <bit>
#include <span>

#include "royalbed/server/web-socket.h"

namespace royalbed::server::detail {

class WebSocketFrameParser
{
public:
    static WebSocketFrame parse(std::span<const uint8_t> data)
    {
        if (data.size() < 2) {
            throw std::runtime_error("Frame too short");
        }

        WebSocketFrame frame;
        size_t pos = 0;

        // Parse first byte
        frame.fin = (data[pos] & 0x80) != 0;
        frame.opcode = data[pos] & 0x0F;
        pos++;

        // Parse second byte
        frame.mask = (data[pos] & 0x80) != 0;
        uint64_t payload_len = data[pos] & 0x7F;
        pos++;

        // Parse extended payload length
        if (payload_len == 126) {
            if (data.size() < pos + 2) {
                throw std::runtime_error("Invalid frame length");
            }
            payload_len = (static_cast<uint64_t>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        } else if (payload_len == 127) {
            if (data.size() < pos + 8) {
                throw std::runtime_error("Invalid frame length");
            }
            payload_len = 0;
            for (int i = 0; i < 8; ++i) {
                payload_len = (payload_len << 8) | data[pos + i];
            }
            pos += 8;
        }

        frame.payloadLength = payload_len;

        // Parse masking key
        if (frame.mask) {
            if (data.size() < pos + 4) {
                throw std::runtime_error("Invalid frame length");
            }
            frame.maskingKey = 0;
            for (int i = 0; i < 4; ++i) {
                frame.maskingKey = (frame.maskingKey << 8) | data[pos + i];
            }
            pos += 4;
        }

        // Check if we have enough data for payload
        if (data.size() < pos + payload_len) {
            throw std::runtime_error("Incomplete frame payload");
        }

        // Extract payload
        frame.payload.assign(data.begin() + pos, data.begin() + pos + payload_len);

        // Unmask payload if needed
        if (frame.mask) {
            for (size_t i = 0; i < frame.payload.size(); ++i) {
                frame.payload[i] ^= ((frame.maskingKey >> (8 * (3 - (i % 4)))) & 0xFF);
            }
        }

        return frame;
    }

    static std::vector<uint8_t> createFrame(bool fin, WebSocketFrame::Opcode opcode, bool mask, uint32_t masking_key,
                                            std::span<const uint8_t> payload)
    {
        std::vector<uint8_t> frame;

        // First byte
        uint8_t first_byte = (fin ? 0x80 : 0x00) | (uint8_t(opcode) & 0x0F);
        frame.push_back(first_byte);

        // Second byte and length
        uint8_t second_byte = mask ? 0x80 : 0x00;
        if (payload.size() < 126) {
            second_byte |= payload.size();
            frame.push_back(second_byte);
        } else if (payload.size() <= 0xFFFF) {
            second_byte |= 126;
            frame.push_back(second_byte);
            frame.push_back((payload.size() >> 8) & 0xFF);
            frame.push_back(payload.size() & 0xFF);
        } else {
            second_byte |= 127;
            frame.push_back(second_byte);
            for (int i = 7; i >= 0; --i) {
                frame.push_back((payload.size() >> (8 * i)) & 0xFF);
            }
        }

        // Masking key
        if (mask) {
            for (int i = 3; i >= 0; --i) {
                frame.push_back((masking_key >> (8 * i)) & 0xFF);
            }
        }

        // Payload (apply mask if needed)
        frame.insert(frame.end(), payload.begin(), payload.end());
        if (mask) {
            for (size_t i = 0; i < payload.size(); ++i) {
                frame[frame.size() - payload.size() + i] ^= (masking_key >> (8 * (3 - (i % 4)))) & 0xFF;
            }
        }

        return frame;
    }
};

}   // namespace royalbed::server::detail
