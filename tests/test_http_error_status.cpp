/**
 * @file       test_http_error_status.cpp
 * @brief      HttpStatusForBody tests — handler error envelopes must map to
 *             HTTP statuses (auth failures were answered 200, walk B1)
 * @date       2026-11-18
 */

#include <gtest/gtest.h>

#include "base/http_error_status.hpp"

#include <boost/beast/http/status.hpp>

namespace
{

using genius::os::HttpStatusForBody;

TEST(HttpStatusForBody, AuthFailureEnvelopesMapToUnauthorized)
{
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})"),
              boost::beast::http::status::unauthorized);
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"INVALID_TOKEN","message":"Token is invalid or expired"}})"),
              boost::beast::http::status::unauthorized);
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"UNAUTHORIZED","message":"Missing or invalid Authorization header"}})"),
              boost::beast::http::status::unauthorized);
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})"),
              boost::beast::http::status::unauthorized);
}

TEST(HttpStatusForBody, SuccessPayloadMapsToOk)
{
    // The login success shape — the pipeline must keep answering 200.
    EXPECT_EQ(HttpStatusForBody(
                  R"({"access_token":"a.b.c","token_type":"bearer","expires_in":3600,"user":{"id":"u1"}})"),
              boost::beast::http::status::ok);
    EXPECT_EQ(HttpStatusForBody(R"({"data":[]})"),
              boost::beast::http::status::ok);
    EXPECT_EQ(HttpStatusForBody(""),
              boost::beast::http::status::ok);
}

TEST(HttpStatusForBody, OtherErrorChannelsStayOk)
{
    // Plain-string error payloads and non-auth codes are not re-statused —
    // only the auth-failure channel changes in this fix.
    EXPECT_EQ(HttpStatusForBody(R"({"error":"no handler found for path: /x"})"),
              boost::beast::http::status::ok);
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})"),
              boost::beast::http::status::ok);
    EXPECT_EQ(HttpStatusForBody(
                  R"({"error":{"code":"INVALID_REQUEST","message":"Email is required"}})"),
              boost::beast::http::status::ok);
    EXPECT_EQ(HttpStatusForBody("not json at all"),
              boost::beast::http::status::ok);
}

} // namespace
