#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <string>
#include <vector>

#include "fmt/format.h"
#include "cmrc/cmrc.hpp"

#include "fmt/ranges.h"
#include "nhope/async/ao-context.h"
#include "nhope/async/future.h"
#include "nhope/async/thread-executor.h"
#include "nhope/io/file.h"
#include "nhope/io/io-device.h"
#include "nhope/io/string-reader.h"

#include "royalbed/common/mime-type.h"
#include "royalbed/server/router.h"
#include "royalbed/server/static-files.h"

namespace royalbed::server {

namespace {
namespace fs = std::filesystem;
using namespace std::literals;

constexpr auto indexHtml = "index.html"sv;

std::string join(std::list<std::string_view> args)
{
    args.remove_if([](auto a) {
        return a.empty();
    });
    return fmt::format("{}", fmt::join(args, "/"));
}

std::optional<std::string> getContentEncodingByExtension(std::string_view filePath)
{
    const auto ext = fs::path(filePath).extension().string();
    if (ext == ".gz") {
        return "gzip";
    }

    return std::nullopt;
}

std::string removeEncoderExtension(std::string_view filePath)
{
    const auto encoderExtensionPos = filePath.rfind('.');
    return std::string(filePath.substr(0, encoderExtensionPos));
}

void publicFile(Router& router, const std::string_view fileName, const std::vector<std::uint8_t>& data,
                std::string_view parentPath)
{
    auto resourcePath = join({parentPath, fileName});
    const auto contentEncoding = getContentEncodingByExtension(resourcePath);
    if (contentEncoding) {
        resourcePath = removeEncoderExtension(resourcePath);
    }
    auto handle = [data, contentEncoding,
                   contentType = std::string(common::mimeTypeForFileName(resourcePath))](RequestContext& ctx) {
        ctx.response.headers["Content-Length"] = std::to_string(data.size());
        ctx.response.headers["Content-Type"] = contentType;

        if (contentEncoding != std::nullopt) {
            ctx.response.headers["Content-Encoding"] = contentEncoding.value();
        };
        ctx.response.body = nhope::StringReader::create(ctx.aoCtx, {(const char*)data.data(), data.size()});
    };
    router.get(resourcePath, handle);
    if (fileName == indexHtml) {
        // Redirect to index page
        router.get(parentPath, handle);
    }
}

void publicFile(Router& router, const cmrc::embedded_filesystem& fs, const cmrc::directory_entry& entry,
                std::string_view parentPath)
{
    const auto& filename = entry.filename();
    auto resourcePath = join({parentPath, filename});
    const auto file = fs.open(resourcePath);
    publicFile(router, filename, {file.begin(), file.end()}, parentPath);
}

void publicDirEntry(Router& router, const cmrc::embedded_filesystem& fs, const cmrc::directory_entry& entry,
                    std::string_view parentPath = "")
{
    if (entry.is_file()) {
        publicFile(router, fs, entry, parentPath);
        return;
    }

    const auto entryPath = join({parentPath, entry.filename()});
    for (const auto& subEntry : fs.iterate_directory(entryPath)) {
        publicDirEntry(router, fs, subEntry, entryPath);
    }
}

void publicDirEntry(Router& router, const std::filesystem::path& fs, const std::filesystem::directory_entry& entry,
                    std::string_view rootFolder, std::string_view parentPath = "")
{
    if (entry.is_regular_file()) {
        const auto& filename = entry.path().filename();
        const auto filePath = fs / filename;
        nhope::ThreadExecutor ex;
        nhope::AOContext ctx(ex);
        const auto dev = nhope::File::open(ctx, filePath.c_str(), nhope::OpenFileMode::ReadOnly);
        const auto data = nhope::readAll(*dev).get();
        std::string_view pp = parentPath;
        if (!parentPath.empty()) {
            pp = parentPath.substr(rootFolder.size());
        }
        publicFile(router, filename.c_str(), data, pp);
        return;
    }
    if (entry.is_directory()) {
        const std::string_view entryPath = entry.path().c_str();
        for (const auto& subEntry : std::filesystem::directory_iterator(entryPath)) {
            publicDirEntry(router, entry, subEntry, rootFolder, entryPath);
        }
    }
}

}   // namespace

Router staticFiles(const cmrc::embedded_filesystem& fs)
{
    Router router;
    for (const auto& entry : fs.iterate_directory("")) {
        publicDirEntry(router, fs, entry);
    }
    return router;
}

Router staticFiles(const std::filesystem::path& fs)
{
    Router router;
    const std::string_view root = fs.c_str();
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(fs)) {
        publicDirEntry(router, fs, entry, root);
    }
    return router;
}

}   // namespace royalbed::server
