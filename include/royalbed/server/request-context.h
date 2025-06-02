#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "royalbed/server/web-socket.h"
#include "spdlog/logger.h"

#include "nhope/async/ao-context.h"

#include "royalbed/server/request.h"
#include "royalbed/server/response.h"

namespace royalbed::server {

class Router;
using RawPathParams = std::vector<std::pair<std::string, std::string>>;

struct RequestContext final
{
    const std::uint64_t num;

    std::shared_ptr<spdlog::logger> log;

    const Router& router;

    std::optional<WebSocketController> webSocket;

    Request request;
    RawPathParams rawPathParams;

    Response response;

    nhope::AOContext aoCtx;
};

}   // namespace royalbed::server
