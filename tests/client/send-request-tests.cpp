#include <memory>
#include <system_error>

#include <gtest/gtest.h>

#include "nhope/async/ao-context-error.h"
#include "nhope/async/ao-context.h"
#include "nhope/async/async-invoke.h"
#include "nhope/async/thread-executor.h"
#include "nhope/io/io-device.h"
#include "nhope/io/pushback-reader.h"
#include "nhope/io/string-reader.h"
#include "nhope/io/string-writter.h"

#include "nhope/io/tcp.h"
#include "royalbed/client/detail/send-request.h"
#include "royalbed/client/request.h"

#include "helpers/iodevs.h"
#include "royalbed/client/uri.h"
#include "royalbed/common/detail/write-headers.h"
#include "royalbed/common/response.h"
#include "royalbed/server/detail/send-response.h"
#include "royalbed/server/response.h"

namespace {

using namespace std::literals;
using namespace royalbed::client;
using namespace royalbed::client::detail;

}   // namespace

TEST(SendRequest, SendReqWithoutBody)   // NOLINT
{
    constexpr auto etalone = "GET /file HTTP/1.1\r\nHeader1: Value1\r\n\r\n"sv;

    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto req = Request{
      .method = "GET",
      .uri = {.path = "/file"},
      .headers =
        {
          {"Header1", "Value1"},
        },
    };

    auto dev = nhope::StringWritter::create(aoCtx);

    const auto n = sendRequest(aoCtx, std::move(req), *dev).get();

    EXPECT_EQ(n, etalone.size());
    EXPECT_EQ(dev->takeContent(), etalone);
}

TEST(SendRequest, SendReqWithBody)   // NOLINT
{
    constexpr auto etalone = "PUT /file%20name HTTP/1.1\r\nContent-Length: 10\r\n\r\n1234567890"sv;

    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto req = Request{
      .method = "PUT",
      .uri = {.path = "/file name"},
      .headers =
        {
          {"Content-Length", "10"},
        },
      .body = nhope::StringReader::create(aoCtx, "1234567890"),
    };

    auto dev = nhope::StringWritter::create(aoCtx);

    const auto n = sendRequest(aoCtx, std::move(req), *dev).get();

    EXPECT_EQ(n, etalone.size());
    EXPECT_EQ(dev->takeContent(), etalone);
}

TEST(SendRequest, IOError)   // NOLINT
{
    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto req = Request{
      .method = "GET",
      .uri = {.path = "/file"},
      .headers =
        {
          {"Header1", "Value1"},
        },
    };

    auto dev = BrokenSock::create(aoCtx);

    auto future = sendRequest(aoCtx, std::move(req), *dev);

    EXPECT_THROW(future.get(), std::system_error);   // NOLINT
}

TEST(SendRequest, Cancel)   // NOLINT
{
    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto req = Request{
      .method = "GET",
      .uri = {.path = "/file"},
      .headers =
        {
          {"Header1", "Value1"},
        },
    };

    auto dev = SlowSock::create(aoCtx);

    auto future = sendRequest(aoCtx, std::move(req), *dev);

    aoCtx.close();

    EXPECT_THROW(future.get(), nhope::AsyncOperationWasCancelled);   // NOLINT
}

TEST(SendRequest, SendAndReceive)   // NOLINT
{
    class ResponseMocReader final : public nhope::PushbackReader
    {
    public:
        explicit ResponseMocReader(nhope::StringWritter& w)
          : m_writer(w)
          , m_aoCtx(m_executor)
        {
            royalbed::common::Response resp;
            resp.status = 201;
            resp.headers = {{"Content-Length", "0"}};
            m_rxBuf = makeResp(resp);
        }

        void unread(gsl::span<const std::uint8_t> bytes) override
        {
            m_unreadBuf.insert(m_unreadBuf.end(), bytes.rbegin(), bytes.rend());
        }

        void read(gsl::span<std::uint8_t> buf, nhope::IOHandler handler) override
        {
            if (!m_unreadBuf.empty()) {
                const auto size = std::min(buf.size(), m_unreadBuf.size());
                const auto bufSpan = gsl::span(m_unreadBuf).last(size);

                std::copy(bufSpan.rbegin(), bufSpan.rend(), buf.begin());
                m_unreadBuf.resize(m_unreadBuf.size() - size);
                handler(nullptr, size);
                return;
            }

            std::copy(m_rxBuf.begin(), m_rxBuf.end(), buf.begin());

            // royalbed::server::detail::sendResponse(m_aoCtx, {}, 0).get();
            handler(nullptr, m_rxBuf.size());
            // detail::sendResponse
        }

        std::string makeResp(const royalbed::server::Response& response) const
        {
            std::string resp;
            writeStartLine(response, resp);
            royalbed::common::detail::writeHeaders(response.headers, resp);
            resp += "\r\n"sv;
            return resp;
        }

        void writeStartLine(const royalbed::server::Response& response, std::string& out) const
        {
            out += "HTTP/1.1 "sv;
            out += std::to_string(response.status);
            out += ' ';
            if (!response.statusMessage.empty()) {
                out += response.statusMessage;
            } else {
                out += royalbed::common::HttpStatus::message(response.status);
            }
            out += "\r\n";
        }

    private:
        nhope::StringWritter& m_writer;
        std::string m_rxBuf;
        std::vector<uint8_t> m_unreadBuf;
        nhope::ThreadExecutor m_executor;
        nhope::AOContext m_aoCtx;
    };

    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto dev = nhope::StringWritter::create(aoCtx);
    ResponseMocReader respReader(*dev);
    auto reader = nhope::PushbackReader::create(aoCtx, respReader);

    const auto response = makeRequest(aoCtx,
                                      Request{
                                        .method = "GET",
                                        .uri = {.path = "/file"},
                                        .headers =
                                          {
                                            {"Header1", "Value1"},
                                          },
                                      },
                                      *dev, *reader)
                            .get();
    EXPECT_EQ(response.status, 201);

    // GOOGLE get example
    // auto tcpSock = nhope::TcpSocket::connect(aoCtx, "142.250.184.238", 80).get();
    // auto r = nhope::PushbackReader::create(aoCtx, *tcpSock);
    // const auto res = makeRequest(aoCtx,
    //                              Request{
    //                                .method = "GET",
    //                                .uri = {.path = "/"},
    //                                .headers =
    //                                  {
    //                                    {"Host", "142.250.184.238:80"},
    //                                  },
    //                              },
    //                              *tcpSock, *r)
    //                    .get();
    // const auto b = nhope::readAll(*res.body).get();
    // EXPECT_EQ(res.status, 200);
}

TEST(SendRequest, SendGoogle)   // NOLINT
{
    nhope::ThreadExecutor executor;
    nhope::AOContext aoCtx(executor);

    auto res = sendRequest(aoCtx,
                           {
                             .method = "GET",
                             .uri = Uri::parse("http://www.google.com/"),
                           })
                 .get();
    const auto b = nhope::readAll(*res.body).get();
    printf((const char*)b.data());
    EXPECT_EQ(res.status, 200);
}
