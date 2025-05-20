#include <cstddef>
#include <string_view>
#include "3rdparty/llhttp/llhttp.h"

#include "nhope/io/io-device.h"
#include "nhope/io/string-reader.h"

#include "royalbed/client/http-error.h"
#include "royalbed/client/request.h"
#include "royalbed/common/detail/write-headers.h"
#include "royalbed/common/detail/body-reader.h"
#include "royalbed/common/response.h"

#include "royalbed/client/detail/send-request.h"

namespace royalbed::client::detail {

namespace {
using namespace std::literals;
using namespace royalbed::common;

constexpr std::size_t receiveBufSize = 4096;

class ResponseReceiver final : public std::enable_shared_from_this<ResponseReceiver>
{
public:
    ResponseReceiver(nhope::AOContext& aoCtx, nhope::PushbackReader& device)
      : m_aoCtx(aoCtx)
      , m_device(device)
      , m_httpParser(std::make_unique<llhttp_t>())
    {
        llhttp_init(m_httpParser.get(), HTTP_RESPONSE, &llhttpSettings);

        //
        m_httpParser->data = this;
    }

    ~ResponseReceiver()
    {
        if (!m_promise.satisfied()) {
            m_promise.setException(std::make_exception_ptr(nhope::AsyncOperationWasCancelled()));
        }
    }

    nhope::Future<Response> start()
    {
        this->readNextPortion();
        return m_promise.future();
    }

private:
    bool processData(std::span<std::uint8_t> data)
    {
        assert(!m_headersComplete);   // NOLINT

        if (!data.empty()) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            llhttp_execute(m_httpParser.get(), reinterpret_cast<char*>(data.data()), data.size());
        } else {
            llhttp_finish(m_httpParser.get());
        }

        if (m_httpParser->error == HPE_OK) {
            return true;
        }

        if (m_httpParser->error != HPE_PAUSED) {
            const auto* reason = llhttp_get_error_reason(m_httpParser.get());
            auto ex = std::make_exception_ptr(HttpError(HttpStatus::BadRequest, reason));
            m_promise.setException(std::move(ex));
            return false;
        }

        // Headers received

        assert(m_headersComplete);   // NOLINT

        m_httpParser->data = nullptr;
        llhttp_resume(m_httpParser.get());

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* beginBody = reinterpret_cast<const std::uint8_t*>(llhttp_get_error_pos(m_httpParser.get()));
        m_device.unread({std::to_address(beginBody), std::to_address(data.end())});

        bool isChunkedBody{false};
        if (auto it = m_response.headers.find("Transfer-Encoding"); it != m_response.headers.end()) {
            isChunkedBody = it->second == "chunked";
        }

        m_response.body = common::detail::BodyReader::create(m_aoCtx, m_device, std::move(m_httpParser), isChunkedBody);

        m_promise.setValue(std::move(m_response));

        return false;
    }

    void readNextPortion()
    {
        m_device.read(m_receiveBuf, [self = shared_from_this()](std::exception_ptr err, std::size_t n) {
            self->m_aoCtx.exec([self, err = std::move(err), n] {
                if (!self->processData(std::span(self->m_receiveBuf.begin(), n))) {
                    return;
                }

                if (err) {
                    self->m_promise.setException(err);
                    return;
                }

                if (n == 0) {
                    auto ex = std::make_exception_ptr(HttpError(HttpStatus::BadRequest));
                    self->m_promise.setException(std::move(ex));
                    return;
                }

                self->readNextPortion();
            });
        });
    }

    static int onUrl(llhttp_t* httpParser, const char* at, std::size_t size)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_url.append(at, size);
        return HPE_OK;
    }

    static int onStatusMessageComplete(llhttp_t* httpParser, const char* at, size_t length)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_response.statusMessage = std::string(at, length);
        return HPE_OK;
    }

    static int onHeaderName(llhttp_t* httpParser, const char* at, std::size_t size)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_curHeaderName.append(at, size);
        return HPE_OK;
    }

    static int onHeaderValue(llhttp_t* httpParser, const char* at, std::size_t size)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_curHeaderValue.append(at, size);
        return HPE_OK;
    }

    static int onHeaderComplete(llhttp_t* httpParser)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);

        assert(self->m_curHeaderName.size() > 0);   // NOLINT

        self->m_response.headers[self->m_curHeaderName] = self->m_curHeaderValue;
        self->m_curHeaderName.clear();
        self->m_curHeaderValue.clear();

        return HPE_OK;
    }

    static int onHeadersComplete(llhttp_t* httpParser)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_headersComplete = true;
        return HPE_PAUSED;
    }

    static int onStatusComplete(llhttp_t* httpParser)
    {
        auto* self = static_cast<ResponseReceiver*>(httpParser->data);
        self->m_response.status = httpParser->status_code;
        return HPE_OK;
    }

    static constexpr llhttp_settings_s llhttpSettings = {
      .on_url = onUrl,
      .on_status = onStatusMessageComplete,
      .on_header_field = onHeaderName,
      .on_header_value = onHeaderValue,

      .on_headers_complete = onHeadersComplete,
      .on_status_complete = onStatusComplete,
      .on_header_value_complete = onHeaderComplete,
    };

    nhope::AOContextRef m_aoCtx;
    nhope::PushbackReader& m_device;

    nhope::Promise<Response> m_promise;

    std::unique_ptr<llhttp_t> m_httpParser;
    std::string m_url;
    std::string m_curHeaderName;
    std::string m_curHeaderValue;
    bool m_headersComplete = false;

    std::array<std::uint8_t, receiveBufSize> m_receiveBuf{};

    Response m_response;
};

void writePath(const Request& req, std::string& out)
{
    out += req.uri.toString();
}

void writeStartLine(const Request& req, std::string& out)
{
    out += req.method;
    out += ' ';
    writePath(req, out);
    out += " HTTP/1.1\r\n"sv;
}

nhope::ReaderPtr makeRequestHeaderStream(nhope::AOContext& aoCtx, const Request& req)
{
    std::string requestHeader;
    writeStartLine(req, requestHeader);
    writeHeaders(req.headers, requestHeader);
    requestHeader += "\r\n"sv;
    return nhope::StringReader::create(aoCtx, std::move(requestHeader));
}

nhope::ReaderPtr makeRequestStream(nhope::AOContext& aoCtx, Request&& request)
{
    if (request.body == nullptr) {
        return makeRequestHeaderStream(aoCtx, request);
    }

    return nhope::concat(aoCtx,                                     //
                         makeRequestHeaderStream(aoCtx, request),   //
                         std::move(request.body));
}

}   // namespace

nhope::Future<std::size_t> sendRequest(nhope::AOContext& aoCtx, Request&& request, nhope::Writter& device)
{
    auto requestStream = makeRequestStream(aoCtx, std::move(request));
    return nhope::copy(*requestStream, device).then([requestStream = std::move(requestStream)](auto n) {
        return n;
    });
}

nhope::Future<common::Response> makeRequest(nhope::AOContext& aoCtx, Request&& request, nhope::Writter& device,
                                            nhope::PushbackReader& reader)
{
    return sendRequest(aoCtx, std::move(request), device).then([&aoCtx, &reader](std::size_t) {
        auto receiver = std::make_shared<ResponseReceiver>(aoCtx, reader);
        return receiver->start();
    });
}

}   // namespace royalbed::client::detail
