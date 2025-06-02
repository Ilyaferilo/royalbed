#pragma once

#include "nhope/async/ao-context.h"
#include "nhope/async/future.h"
#include "nhope/io/io-device.h"
#include "nhope/utils/detail/fast-pimpl.h"
#include "spdlog/logger.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace royalbed::server {

struct WebSocketFrame
{
    enum Opcode : uint8_t
    {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA
    };

    bool fin;
    uint8_t opcode;
    bool mask;
    uint64_t payloadLength;
    uint32_t maskingKey;
    std::vector<uint8_t> payload;
};

class WebSocketController
{
public:
    static std::vector<std::uint8_t> makeHandShake(std::string_view clientKey);

    explicit WebSocketController(nhope::AOContext& ctx, nhope::Reader& r, nhope::Writter& w,
                                 std::shared_ptr<spdlog::logger> l);
    ~WebSocketController();

    nhope::Future<void> waitForClose();
    void close();

    nhope::Future<std::vector<uint8_t>> readFrame();

    nhope::Future<void> writeFrame(const std::string& payload);
    nhope::Future<void> writeFrame(const std::vector<uint8_t>& payload);

private:
    class Impl;
    nhope::detail::FastPimpl<Impl, 65536> m_pimpl;
};

}   // namespace royalbed::server
