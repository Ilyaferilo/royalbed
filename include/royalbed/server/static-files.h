#pragma once

#include "royalbed/server/router.h"
#include <filesystem>

namespace cmrc {
class embedded_filesystem;
}

namespace royalbed::server {

Router staticFiles(const cmrc::embedded_filesystem& fs);

Router staticFiles(const std::filesystem::path& fs);

}   // namespace royalbed::server
