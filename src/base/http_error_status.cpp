/**
 * @file       http_error_status.cpp
 * @brief      Handler-body to HTTP status mapping implementation
 * @date       2026-11-18
 */

#include "base/http_error_status.hpp"

#include <nlohmann/json.hpp>

namespace genius::os
{

namespace
{

/// Auth-failure codes that must reach the client as HTTP 401. A handler's
/// only failure channel is the error envelope; the middleware already
/// 401s its own rejections, these are the codes the auth handlers emit
/// for bad credentials and bad/missing tokens of the CALLER. A handler
/// reporting some other credential's failure must use a code outside
/// this list (a 401 here makes the client refresh and drop its session).
bool IsAuthFailureCode(const std::string& code)
{
    return code == "INVALID_CREDENTIALS" || code == "INVALID_TOKEN" ||
           code == "UNAUTHORIZED" || code == "TOKEN_ERROR";
}

/// Permission-denied code that must reach the client as HTTP 403 — a
/// refusal about the caller's standing, not their session, so it must
/// not ride the 401 channel (a refresh cannot cure it).
bool IsPermissionDeniedCode(const std::string& code)
{
    return code == "FORBIDDEN";
}

} // namespace

boost::beast::http::status HttpStatusForBody(const std::string& body)
{
    // An error envelope is a JSON object, so a body whose first
    // non-whitespace byte is not '{' can never carry one — skip the parse
    // instead of paying it on every large non-envelope payload.
    const size_t start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || body[start] != '{')
    {
        return boost::beast::http::status::ok;
    }
    try
    {
        const nlohmann::json parsed = nlohmann::json::parse(body);
        if (!parsed.is_object() || !parsed.contains("error"))
        {
            return boost::beast::http::status::ok;
        }
        const nlohmann::json& error = parsed.at("error");
        if (!error.is_object() || !error.contains("code") ||
            !error.at("code").is_string())
        {
            return boost::beast::http::status::ok;
        }
        const std::string code = error.at("code").get<std::string>();
        if (IsAuthFailureCode(code))
        {
            return boost::beast::http::status::unauthorized;
        }
        if (IsPermissionDeniedCode(code))
        {
            return boost::beast::http::status::forbidden;
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // Non-JSON body — keep the legacy 200 passthrough.
    }
    return boost::beast::http::status::ok;
}

} // namespace genius::os
