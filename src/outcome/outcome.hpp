/**
 * @file       outcome.hpp
 * @brief      Outcome library wrapper — re-exports libp2p's outcome types
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */
#ifndef GENIUS_AI_BOSS_OUTCOME_HPP
#define GENIUS_AI_BOSS_OUTCOME_HPP

#include <libp2p/outcome/outcome.hpp>

namespace outcome
{
using libp2p::outcome::failure;
using libp2p::outcome::result;
using libp2p::outcome::success;
} // namespace outcome

#endif // GENIUS_AI_BOSS_OUTCOME_HPP
