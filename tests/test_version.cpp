#include <gtest/gtest.h>
#include <neojson/json.hpp>

#include <string_view>

namespace {

TEST(VersionTest, MatchesProjectVersion) {
    EXPECT_EQ(NEOJSON_VERSION_MAJOR, 0);
    EXPECT_EQ(NEOJSON_VERSION_MINOR, 1);
    EXPECT_EQ(NEOJSON_VERSION_PATCH, 0);
    EXPECT_EQ(std::string_view(NEOJSON_VERSION_STRING), "0.1.0");
}

}  // namespace
