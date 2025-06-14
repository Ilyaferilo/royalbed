#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <memory>
#include <string_view>

#include "3rdparty/llhttp/llhttp.h"
#include "nhope/async/ao-context.h"
#include "nhope/io/pushback-reader.h"

#include "royalbed/common/http-error.h"
#include "royalbed/common/http-status.h"

#include "royalbed/common/detail/body-reader.h"

namespace royalbed::common::detail {
namespace {

class BodyReaderImpl final : public BodyReader
{
public:
    BodyReaderImpl(nhope::AOContextRef& aoCtx, nhope::PushbackReader& device, std::unique_ptr<llhttp_t> httpParser,
                   bool isChunked)
      : m_aoCtxRef(aoCtx)
      , m_device(device)
      , m_isChunked(isChunked)
      , m_httpParser(std::move(httpParser))
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        m_httpParser->settings = const_cast<llhttp_settings_s*>(&llhttpSettings);
        m_httpParser->data = this;
    }

    void read(gsl::span<std::uint8_t> buf, nhope::IOHandler handler) override
    {
        if (m_eof) {
            m_aoCtxRef.exec([handler = std::move(handler)] {
                handler(nullptr, 0);
            });
            return;
        }

        m_device.read(buf, [this, aoCtxRef = m_aoCtxRef, buf, handler = std::move(handler)](auto err, auto n) mutable {
            aoCtxRef.exec([this, buf, err, n, handler = std::move(handler)] {
                if (err) {
                    handler(std::move(err), n);
                    return;
                }

                if (m_isChunked) {
                    if (m_leftProcessedChunkSize != 0) {
                        const int l = m_leftProcessedChunkSize - n;
                        if (l >= 0) {
                            m_leftProcessedChunkSize -= n;
                            handler(nullptr, n);
                            return;
                        }
                        m_device.unread(buf.subspan(m_leftProcessedChunkSize));
                        handler(nullptr, m_leftProcessedChunkSize);
                        m_leftProcessedChunkSize = 0;
                        return;
                    }

                    constexpr std::string_view crlf = "\r\n";
                    const auto it = std::find_first_of(buf.begin(), buf.end(), crlf.begin(), crlf.end());
                    if (it == buf.end()) {
                        handler(
                          std::make_exception_ptr(HttpError(HttpStatus::BadRequest, "incorrect body chunk received")),
                          n);
                        return;
                    }
                    const auto dataPos = std::distance(buf.begin(), it + crlf.size());
                    std::size_t chunkDataSize = std::strtoul((const char*)buf.data(), nullptr, 16);
                    if (chunkDataSize == 0) {
                        m_eof = true;
                        handler(nullptr, 0);
                        return;
                    }
                    if (chunkDataSize > n) {
                        // received incomplete chunk
                        m_leftProcessedChunkSize = (int)chunkDataSize;
                        chunkDataSize = n - dataPos;
                        m_leftProcessedChunkSize -= (int)chunkDataSize;
                        std::memmove(buf.data(), buf.data() + dataPos, chunkDataSize);
                    } else {
                        // received full chunk
                        m_device.unread(buf.subspan(dataPos + chunkDataSize));
                        std::memmove(buf.data(), buf.data() + dataPos, chunkDataSize);
                    }
                    handler(nullptr, chunkDataSize);
                } else {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                    llhttp_execute(m_httpParser.get(), reinterpret_cast<const char*>(buf.data()), n);
                    if (n == 0) {
                        llhttp_finish(m_httpParser.get());
                    }

                    if (m_httpParser->error != HPE_OK && m_httpParser->error != HPE_PAUSED) {
                        const auto* reason = llhttp_get_error_reason(m_httpParser.get());
                        handler(std::make_exception_ptr(HttpError(HttpStatus::BadRequest, reason)), n);
                        return;
                    }

                    if (n > m_bodyPieceSize) {
                        const auto remains = buf.subspan(m_bodyPieceSize, n - m_bodyPieceSize);
                        m_device.unread(remains);
                    }

                    handler(nullptr, m_bodyPieceSize);
                }
            });
        });
    }

private:
    static int onBodyData(llhttp_t* httpParser, const char* /*at*/, std::size_t size)
    {
        auto* self = static_cast<BodyReaderImpl*>(httpParser->data);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        self->m_bodyPieceSize = size;
        return HPE_OK;
    }

    static int onMessageComplete(llhttp_t* httpParser)
    {
        auto* self = static_cast<BodyReaderImpl*>(httpParser->data);
        self->m_eof = true;
        return HPE_PAUSED;
    }

    static constexpr llhttp_settings_s llhttpSettings = {
      .on_body = onBodyData,
      .on_message_complete = onMessageComplete,
    };

    nhope::AOContextRef m_aoCtxRef;
    nhope::PushbackReader& m_device;

    const bool m_isChunked;

    int m_leftProcessedChunkSize{};

    std::unique_ptr<llhttp_t> m_httpParser;
    std::size_t m_bodyPieceSize = 0;
    bool m_eof = false;
};

}   // namespace

BodyReaderPtr BodyReader::create(nhope::AOContextRef& aoCtx, nhope::PushbackReader& device,
                                 std::unique_ptr<llhttp_t> httpParser, bool isChunked)
{
    return std::make_unique<BodyReaderImpl>(aoCtx, device, std::move(httpParser), isChunked);
}

}   // namespace royalbed::common::detail
