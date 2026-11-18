/**
 * @file       http_error_status.hpp
 * @brief      Handler-body to HTTP status mapping for the response pipeline
 * @date       2026-11-18
 *
 * Handlers registered with PluginManager return only a JSON string — there
 * is no status channel. The response pipeline in main.cpp used to answer
 * HTTP 200 for every non-empty body, so auth failures came back as 200 with
 * an error envelope and the admin login rendered nothing (walk
 * 2026-11-17 B1). This header exposes the one mapping the pipeline applies.
 */

#pragma once

#include <boost/beast/http/status.hpp>
#include <string>

namespace genius::os
{

///
/// Map a handler-returned body to the HTTP status for the response.
///
/// A body that parses as an object carrying {"error": {"code": ...}}
/// whose code is an auth failure (INVALID_CREDENTIALS, INVALID_TOKEN,
/// UNAUTHORIZED, TOKEN_ERROR) maps to 401 unauthorized. Everything else —
/// success payloads, other error codes, non-JSON bodies — maps to 200 ok:
/// only the auth-failure channel is re-statused, so no handler success
/// shape changes.
///
/// \param body  Handler-returned JSON (or any) string
/// \return      The HTTP status the response pipeline should send
///
boost::beast::http::status HttpStatusForBody(const std::string& body);

} // namespace genius::os
