/**
 * @file       json.hpp
 * @brief      JSON facade for new hand-written code — backed by Boost.JSON.
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 *
 * All NEW code includes this file. Uses Boost.JSON.
 * Generated model code uses <nlohmann/json.hpp> (redirected to real nlohmann).
 * To switch the underlying library, only this file changes.
 */
#ifndef GENIUS_AI_BOSS_JSON_HPP
#define GENIUS_AI_BOSS_JSON_HPP

#include <boost/json.hpp>

///
/// JSON alias for new code. Use json::parse(), json::serialize(), etc.
///
using json = boost::json::value;

#endif // GENIUS_AI_BOSS_JSON_HPP
