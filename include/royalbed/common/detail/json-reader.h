#pragma once

#include "nhope/io/io-device.h"
#include "nlohmann/json.hpp"
#include <string>

namespace royalbed::common::detail {

class StringReader : public nhope::Reader
{
    const std::string m_str;
    std::size_t m_pos = 0;

public:
    explicit StringReader(std::string&& str)
      : m_str(std::move(str))
    {}

    std::size_t size() const noexcept
    {
        return m_str.size();
    }

    void read(gsl::span<std::uint8_t> buf, nhope::IOHandler handler) override
    {
        const auto tail = gsl::span(m_str).subspan(m_pos);
        const auto n = std::min(tail.size(), buf.size());
        if (n == 0) {
            handler(nullptr, 0);
            return;
        }

        std::memcpy(buf.data(), tail.data(), n);
        m_pos += n;

        handler(nullptr, n);
    }
};

class JSONReader final : public StringReader
{
public:
    explicit JSONReader(const nlohmann::json& o)
      : StringReader(o.dump())
    {}
};

}   // namespace royalbed::common::detail
