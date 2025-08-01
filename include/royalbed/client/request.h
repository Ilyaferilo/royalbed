#pragma once

#include "royalbed/common/request.h"
#include "royalbed/client/headers.h"
#include "royalbed/client/uri.h"
#include "royalbed/common/response.h"

namespace royalbed::client {

using common::Request;

/**
 * @brief Установит соединение с сервером и отправит запрос
 *
 * @param aoCtx
 * @param request
 * @return nhope::Future<common::Response>
 */
nhope::Future<common::Response> sendRequest(nhope::AOContext& aoCtx, Request&& request);

}   // namespace royalbed::client
