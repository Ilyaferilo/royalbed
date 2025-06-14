#include "fmt/core.h"
#include "nhope/async/ao-context.h"
#include "nhope/async/future.h"
#include "nhope/async/timer.h"
#include "nhope/io/io-device.h"
#include "nhope/utils/base64.h"
#include <array>
#include <exception>
#include <openssl/sha.h>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "royalbed/server/web-socket.h"
#include "royalbed/server/detail/web-socket-frame.h"
#include "spdlog/logger.h"

namespace royalbed::server {

namespace {
using namespace std::literals;
std::string sha1(const std::string& input, std::size_t iterations = 1) noexcept
{
    std::string hash;

    hash.resize(160 / 8);
    SHA1(reinterpret_cast<const unsigned char*>(&input[0]), input.size(), reinterpret_cast<unsigned char*>(&hash[0]));

    for (std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char*>(&hash[0]), hash.size(), reinterpret_cast<unsigned char*>(&hash[0]));

    return hash;
}

}   // namespace

class WebSocketController::Impl
{
    nhope::AOContext m_ctx;
    nhope::Reader& m_reader;
    nhope::Writter& m_writer;

    nhope::Promise<void> m_closePromise;
    nhope::Promise<void> m_pingPromise;
    std::optional<nhope::Promise<std::vector<uint8_t>>> m_nextFramePromise;

    std::vector<uint8_t> m_receivedPayload;

    std::shared_ptr<spdlog::logger> log;

    bool m_isClosed = false;
    std::array<std::uint8_t, 65000> m_buf{};

public:
    explicit Impl(nhope::AOContext& ctx, nhope::Reader& r, nhope::Writter& w, std::shared_ptr<spdlog::logger> l)
      : m_ctx(ctx)
      , m_reader(r)
      , m_writer(w)
    {
        log = l;

        m_pingPromise.setValue();

        nhope::setInterval(m_ctx, 15s, [this](auto) {
            if (m_isClosed) {
                return false;
            }
            doPing();
            return true;
        });
        readFrame();
    }

    nhope::Future<void> waitClose()
    {
        return m_closePromise.future();
    }

    void close()
    {
        if (!m_isClosed) {
            m_isClosed = true;
            doClose();
        }
    }

    nhope::Future<void> writePayload(WebSocketFrame::Opcode c, std::span<const uint8_t> data)
    {
        return nhope::write(m_writer, detail::WebSocketFrameParser::createFrame(true, c, false, 0, data))
          .then(m_ctx, [](auto) {});
    }

    nhope::Future<std::vector<uint8_t>> readPayload()
    {
        return m_nextFramePromise.emplace().future();
    }

private:
    void readFrame()
    {
        m_reader.read(m_buf, [this](std::exception_ptr ex, std::size_t s) {
            if (ex != nullptr) {
                m_closePromise.setException(std::move(ex));
                return;
            }
            try {
                const WebSocketFrame frame = detail::WebSocketFrameParser::parse({m_buf.begin(), s});

                switch (frame.opcode) {
                case WebSocketFrame::Opcode::CLOSE: {
                    // закрытие соединения
                    m_isClosed = true;
                    m_closePromise.setValue();
                    return;
                }
                case WebSocketFrame::Opcode::CONTINUATION:
                    // Добавляем данные к текущему сообщению
                    m_receivedPayload.insert(m_receivedPayload.end(), frame.payload.begin(), frame.payload.end());
                    if (frame.fin) {
                        processPayload();
                    }
                    break;
                case WebSocketFrame::Opcode::TEXT:
                case WebSocketFrame::Opcode::BINARY: {
                    // Начало нового сообщения
                    m_receivedPayload = frame.payload;
                    if (frame.fin) {
                        processPayload();
                    }
                    break;
                }
                case WebSocketFrame::Opcode::PING:
                    doPong(frame.payload);
                    break;
                case WebSocketFrame::Opcode::PONG:
                    m_pingPromise.setValue();
                    break;
                }

            } catch (const std::exception& e) {
                m_isClosed = true;
                m_closePromise.setException(std::make_exception_ptr(e));
                return;
            }

            readFrame();
        });
    }

    void processPayload()
    {
        log->debug("WebSocketController::processPayload before {}", m_nextFramePromise.has_value());
        if (m_nextFramePromise.has_value()) {
            auto tmp = std::move(m_nextFramePromise);
            m_nextFramePromise = std::nullopt;
            tmp->setValue(m_receivedPayload);
        }
        log->debug("WebSocketController::processPayload after {}", m_nextFramePromise.has_value());
    }

    void doPing()
    {
        if (!m_pingPromise.satisfied()) {
            m_pingPromise.setException(std::make_exception_ptr(std::runtime_error("не получен пинг от клиента")));
            return;
        }
        m_pingPromise = nhope::Promise<void>();
        m_pingPromise.future().fail(m_ctx, [this](auto ex) {
            m_isClosed = true;
            m_closePromise.setException(std::move(ex));
        });

        static constexpr std::string_view pingFrame = "ping";
        const auto data = royalbed::server::detail::WebSocketFrameParser::createFrame(
          true, WebSocketFrame::Opcode::PING, false, 0,
          {reinterpret_cast<const std::uint8_t*>(pingFrame.data()), pingFrame.size()});
        m_writer.write(data, [](std::exception_ptr, std::size_t) {});
    }

    void doPong(const std::vector<std::uint8_t>& payload)
    {
        const auto data = royalbed::server::detail::WebSocketFrameParser::createFrame(
          true, WebSocketFrame::Opcode::PONG, false, 0, payload);
        m_writer.write(data, [](std::exception_ptr, std::size_t) {});
    }

    void doClose()
    {
        static constexpr std::array<std::uint8_t, 2> closeFrame = {0x03, 0xE8};
        const auto data = royalbed::server::detail::WebSocketFrameParser::createFrame(
          true, WebSocketFrame::Opcode::CLOSE, false, 0, closeFrame);
        m_writer.write(data, [](std::exception_ptr, std::size_t) {});

        // таймаут ожидания получения ответного фрейма на закрытие соединения
        nhope::setTimeout(m_ctx, 4s, [this](auto) {
            if (!m_closePromise.satisfied()) {
                m_closePromise.setValue();
            }
        });
    }
};

std::vector<std::uint8_t> WebSocketController::makeHandShake(std::string_view clientKey)
{
    const auto sha1r = sha1(fmt::format("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", clientKey));
    const auto resp = fmt::format("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
                                  "Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
                                  nhope::toBase64({(const std::uint8_t*)sha1r.data(), sha1r.size()}));
    return {resp.begin(), resp.end()};
}

WebSocketController::WebSocketController(nhope::AOContext& ctx, nhope::Reader& r, nhope::Writter& w,
                                         std::shared_ptr<spdlog::logger> l)
  : m_pimpl(ctx, r, w, l)
{}

WebSocketController::~WebSocketController() = default;

nhope::Future<std::vector<uint8_t>> WebSocketController::readFrame()
{
    return m_pimpl->readPayload();
}

nhope::Future<void> WebSocketController::writeFrame(const std::string& payload)
{
    return m_pimpl->writePayload(WebSocketFrame::TEXT,
                                 {reinterpret_cast<const uint8_t*>(payload.data()), payload.size()});
}

nhope::Future<void> WebSocketController::writeFrame(const std::vector<uint8_t>& payload)
{
    return m_pimpl->writePayload(WebSocketFrame::BINARY, payload);
}

void WebSocketController::close()
{
    m_pimpl->close();
}

nhope::Future<void> WebSocketController::waitForClose()
{
    return m_pimpl->waitClose();
}

}   // namespace royalbed::server
