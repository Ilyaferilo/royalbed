#include <gtest/gtest.h>

#include "royalbed/common/uri.h"

using namespace royalbed::common;

TEST(Uri, uriEscape)   // NOLINT
{
    EXPECT_EQ(uriEscape("hello, /world"), "hello%2c%20%2fworld");
    EXPECT_EQ(uriEscape("hello, /world", UriEscapeMode::Path), "hello%2c%20/world");
    EXPECT_EQ(uriEscape("hello, /world", UriEscapeMode::Query), "hello%2c+%2fworld");

    EXPECT_EQ(uriEscape("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~"),
              "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~");

    EXPECT_EQ(uriEscape(""), "");
}

TEST(Uri, uriUnescape)   // NOLINT
{
    EXPECT_EQ(uriUnescape("hello%2c%20%2fworld"), "hello, /world");
    EXPECT_EQ(uriUnescape("hello%2c%20/world", UriEscapeMode::Path), "hello, /world");
    EXPECT_EQ(uriUnescape("hello%2c+%2fworld", UriEscapeMode::Query), "hello, /world");

    EXPECT_EQ(uriUnescape("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~"),
              "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_.~");

    EXPECT_EQ(uriUnescape(""), "");

    EXPECT_THROW(uriUnescape("hello%"), UriParseError);     // NOLINT
    EXPECT_THROW(uriUnescape("hello%2"), UriParseError);    // NOLINT
    EXPECT_THROW(uriUnescape("hello%2g"), UriParseError);   // NOLINT
    EXPECT_THROW(uriUnescape("hello+"), UriParseError);     // NOLINT
    EXPECT_THROW(uriUnescape("hello/"), UriParseError);     // NOLINT
}

TEST(Uri, toString_relative)   // NOLINT
{
    {
        const auto uri = Uri{};
        ASSERT_EQ(uri.toString(), "/");
    }

    {
        const auto uri = Uri{
          .path = "/a",
          .query =
            {
              {"k", "v"},
            },
        };
        ASSERT_EQ(uri.toString(), "/a?k=v");
    }

    {
        const auto uri = Uri{
          .path = "/a",
          .fragment = "fragment",
        };
        ASSERT_EQ(uri.toString(), "/a#fragment");
    }

    {
        const auto uri = Uri{
          .path = "/a/b/c d",
          .query =
            {
              {"key", "value1"},
              {"key2/ ", "value2/ "},
            },
          .fragment = "frag/ ment",
        };
        ASSERT_TRUE(uri.isRelative());

        ASSERT_EQ(uri.toString(), "/a/b/c%20d?key=value1&key2%2f+=value2%2f+#frag%2f%20ment");
    }
}

TEST(Uri, parseRelative)   // NOLINT
{
    {
        const auto uri = Uri::parseRelative("/a/b/c%20d");

        EXPECT_TRUE(uri.scheme.empty());
        EXPECT_TRUE(uri.host.empty());
        EXPECT_EQ(uri.port, 0);
        EXPECT_EQ(uri.path, "/a/b/c d");
        EXPECT_TRUE(uri.query.empty());
        EXPECT_TRUE(uri.fragment.empty());
    }

    {
        const auto uri = Uri::parseRelative("/a/b/c%20d?key");

        EXPECT_EQ(uri.path, "/a/b/c d");
        EXPECT_EQ(uri.query, Uri::Query({{"key", ""}}));
        EXPECT_TRUE(uri.fragment.empty());
    }

    {
        const auto uri = Uri::parseRelative("/a/b/c%20d?key&&&&#");
        EXPECT_EQ(uri.path, "/a/b/c d");
        EXPECT_EQ(uri.query, Uri::Query({{"key", ""}}));
        EXPECT_TRUE(uri.fragment.empty());
    }

    {
        const auto uri = Uri::parseRelative("/a/b/c%20d?key=value1&key2%2f+=value2%2f+#frag%2f%20ment");

        EXPECT_EQ(uri.path, "/a/b/c d");
        EXPECT_EQ(uri.query, Uri::Query({{"key", "value1"}, {"key2/ ", "value2/ "}}));
        EXPECT_EQ(uri.fragment, "frag/ ment");
    }
}

TEST(Uri, parse)   // NOLINT
{
    const auto uri = Uri::parse("http://172.16.23.45:8080/path/to/resource?param=value&param2=value2#fragment");

    EXPECT_EQ(uri.scheme, "http");
    EXPECT_EQ(uri.host, "172.16.23.45");
    EXPECT_EQ(uri.port, 8080);
    EXPECT_EQ(uri.path, "/path/to/resource");
    EXPECT_EQ(uri.query.size(), 2);
    EXPECT_EQ(uri.query[0].first, "param");
    EXPECT_EQ(uri.query[0].second, "value");
    EXPECT_EQ(uri.query[1].first, "param2");
    EXPECT_EQ(uri.query[1].second, "value2");
    EXPECT_EQ(uri.fragment, "fragment");
    EXPECT_FALSE(uri.isRelative());
}
