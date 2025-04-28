#pragma once

#include <cstddef>

#include "nhope/async/ao-context.h"
#include "nhope/async/future.h"
#include "nhope/io/io-device.h"

#include "nhope/io/pushback-reader.h"
#include "royalbed/client/request.h"
#include "royalbed/common/response.h"

namespace royalbed::client::detail {

nhope::Future<std::size_t> sendRequest(nhope::AOContext& aoCtx, Request&& request, nhope::Writter& device);

nhope::Future<common::Response> makeRequest(nhope::AOContext& aoCtx, Request&& request, nhope::Writter& device,
                                            nhope::PushbackReader& reader);

}   // namespace royalbed::client::detail
