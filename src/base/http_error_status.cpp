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
/// for bad credentials and bad/missing tokens.
bool IsAuthFailureCode(const std::string& code)
{
    return code == "INVALID_CREDENTIALS" || code == "INVALID_TOKEN" ||
           code == "UNAUTHORIZED" || code == "TOKEN_ERROR";
}

} // namespace

boost::beast::http::status HttpStatusForBody(const std::string& body)
{
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
        if (IsAuthFailureCode(error.at("code").get<std::string>()))
        {
            return boost::beast::http::status::unauthorized;
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // Non-JSON body — keep the legacy 200 passthrough.
    }
    return boost::beast::http::status::ok;
}

} // namespace genius::os
