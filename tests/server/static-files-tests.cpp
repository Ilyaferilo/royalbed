#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "cmrc/cmrc.hpp"
#include "nhope/utils/scope-exit.h"
#include "spdlog/spdlog.h"

#include "nhope/async/ao-context.h"
#include "nhope/async/thread-executor.h"
#include "nhope/io/io-device.h"

#include "royalbed/server/router.h"
#include "royalbed/server/static-files.h"
#include "royalbed/server/swagger.h"

#include "helpers/logger.h"

CMRC_DECLARE(royalbed::swagger);

namespace {
using namespace std::literals;
using namespace royalbed::server;

std::vector<char> genRandom(std::size_t size)
{
    static std::mt19937 gen;   // NOLINT(cert-msc32-c,cert-msc51-cpp)

    std::vector<char> retval;
    retval.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        retval.push_back(static_cast<char>(gen()));
    }
    return retval;
}

const auto emptyFileData = std::vector<char>();

constexpr auto smallFileSize = 235;
const auto smallFileData = genRandom(smallFileSize);   // NOLINT(cert-err58-cpp)

const auto openApiFileData = genRandom(smallFileSize);   // NOLINT(cert-err58-cpp)

constexpr auto bigFileSize = 2 * 1024 * 1024 + 235;
const auto bigFileData = genRandom(bigFileSize);   // NOLINT(cert-err58-cpp)

const auto encodedFileData = std::vector<char>();

cmrc::embedded_filesystem testFs()
{
    using namespace cmrc::detail;

    static auto rootDir = directory();
    static auto rootFod = file_or_directory{rootDir};
    static auto folder1 = rootDir.add_subdir("folder1");
    static auto folder2 = rootDir.add_subdir("folder2");

    static auto rootIndex = index_type{
      {"", &rootFod},
      {"folder1", &folder1.index_entry},
      {"folder2", &folder2.index_entry},
      {
        "empty-file.json",
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        rootDir.add_file("empty-file.json", emptyFileData.data(), emptyFileData.data() + emptyFileData.size()),
      },
      {
        "folder2/small-file.bin",
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        folder2.directory.add_file("small-file.bin", smallFileData.data(), smallFileData.data() + smallFileData.size()),
      },
      {
        "folder2/big-file.bin",
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        folder2.directory.add_file("big-file.bin", bigFileData.data(), bigFileData.data() + bigFileData.size()),
      },
      {
        "folder2/encoded-file.js.gz",
        folder2.directory.add_file("encoded-file.js.gz", encodedFileData.data(),
                                   // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                                   encodedFileData.data() + encodedFileData.size()),
      },
      {
        "folder2/openapi.yml",
        folder2.directory.add_file("openapi.yml", openApiFileData.data(),
                                   // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                                   openApiFileData.data() + openApiFileData.size()),
      },

    };

    return cmrc::embedded_filesystem(rootIndex);
}

bool eq(std::span<const char> v1, std::span<const std::uint8_t> v2)
{
    if (v1.size() != v2.size()) {
        return false;
    }

    return memcmp(v1.data(), v2.data(), v2.size()) == 0;
}

std::tuple<std::filesystem::path, std::vector<char>, std::vector<char>> testLocalFs()
{
    std::filesystem::path dir =
      // std::filesystem::temp_directory_path() /
      "./local-fs-test";
    std::filesystem::create_directory(dir);
    std::filesystem::create_directory(dir / "folder1");

    const auto data = genRandom(42);
    const auto data2 = genRandom(42);
    {
        std::ofstream file(dir / ("test1"));
        file.write(data.data(), data.size());
    }

    {
        std::ofstream file(dir / "folder1" / "test2");
        file.write(data2.data(), data2.size());
    }

    return {dir, data, data2};
}

}   // namespace

TEST(StaticFiles, getFiles)   // NOLINT
{
    const auto router = staticFiles(testFs());

    struct TestRec
    {
        const std::string path;
        const std::string etalonContentType;
        const std::span<const char> etalonData;
        const std::string contentEncoding;
    };
    const auto testRecs = std::vector<TestRec>{
      {"/empty-file.json", "application/json", emptyFileData},
      {"/folder2/small-file.bin", "application/octet-stream", smallFileData},
      {"/folder2/big-file.bin", "application/octet-stream", bigFileData},
      {"/folder2/encoded-file.js", "application/javascript; charset=utf-8", encodedFileData, "gzip"},
      {"/folder2/openapi.yml", "application/yaml", openApiFileData},
    };

    nhope::ThreadExecutor th;
    nhope::AOContext aoCtx(th);

    for (const auto& rec : testRecs) {
        RequestContext reqCtx{
          .num = 1,
          .log = nullLogger(),
          .router = router,
          .aoCtx = nhope::AOContext(aoCtx),
        };
        router.route("GET", rec.path).handler(reqCtx).get();

        EXPECT_EQ(reqCtx.response.status, HttpStatus::Ok);

        EXPECT_EQ(reqCtx.response.headers["Content-Type"], rec.etalonContentType);
        EXPECT_EQ(reqCtx.response.headers["Content-Length"], std::to_string(rec.etalonData.size()));
        EXPECT_EQ(reqCtx.response.headers["Content-Encoding"], rec.contentEncoding);

        const auto body = nhope::readAll(*reqCtx.response.body).get();
        EXPECT_TRUE(eq(rec.etalonData, body));
    }
}

TEST(Swagger, Api)   // NOLINT
{
    nhope::ThreadExecutor th;
    nhope::AOContext aoCtx(th);
    Router router{};
    RequestContext reqCtx{
      .num = 1,
      .log = nullLogger(),
      .router = router,
      .aoCtx = nhope::AOContext(aoCtx),
    };

    swagger(router, testFs(), "folder2/openapi.yml");

    {
        router.route("GET", "/swagger/doc-api").handler(reqCtx).get();
        const auto body = nhope::readAll(*reqCtx.response.body).get();
        EXPECT_TRUE(eq(openApiFileData, body));
    }

    {
        router.route("GET", "swagger/index.html").handler(reqCtx).get();
        auto swaggerFs = cmrc::royalbed::swagger::get_filesystem();
        const auto htmlBody = swaggerFs.open("swagger/index.html");
        const auto body = nhope::readAll(*reqCtx.response.body).get();
        EXPECT_TRUE(eq(std::span{htmlBody.begin(), htmlBody.end()}, body));
        EXPECT_EQ(reqCtx.response.headers["Content-Type"], "text/html; charset=utf-8");
    }
}

TEST(StaticFiles, getSystemFiles)   // NOLINT
{
    nhope::ThreadExecutor th;
    nhope::AOContext aoCtx(th);
    const auto [folder, d1, d2] = testLocalFs();

    nhope::ScopeExit closeDir([folder]() {
        std::filesystem::remove_all(folder);
    });
    const auto router = staticFiles(folder);
    {
        RequestContext reqCtx{
          .num = 1,
          .log = nullLogger(),
          .router = router,
          .aoCtx = nhope::AOContext(aoCtx),
        };

        router.route("GET", "/test1").handler(reqCtx).get();
        EXPECT_EQ(reqCtx.response.status, HttpStatus::Ok);
        const auto body = nhope::readAll(*reqCtx.response.body).get();
        EXPECT_TRUE(eq(d1, body));
    }

    {
        RequestContext reqCtx{
          .num = 2,
          .log = nullLogger(),
          .router = router,
          .aoCtx = nhope::AOContext(aoCtx),
        };

        router.route("GET", "/folder1/test2").handler(reqCtx).get();
        EXPECT_EQ(reqCtx.response.status, HttpStatus::Ok);
        const auto body = nhope::readAll(*reqCtx.response.body).get();
        EXPECT_TRUE(eq(d2, body));
    }
}
