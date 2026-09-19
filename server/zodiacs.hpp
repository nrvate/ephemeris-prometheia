// SPDX-License-Identifier: GPL-2.0-or-later
//
// The zodiac tokens this server serves (protocol v4 A.11), and the engine's
// mode for each: prometheiad's WELCOME and its profile mapping, and the agent
// tools (server/json_tools.hpp), read this one list.
#ifndef PROMETHEIA_SERVER_ZODIACS_HPP
#define PROMETHEIA_SERVER_ZODIACS_HPP

#include "prometheia/engine.hpp"

namespace prometheia::server {

// The A.11 zodiac tokens this server serves, and the engine's mode for each:
// WELCOME lists exactly these (A.3 0x0007), and a profile maps through them.
// `user` is served too, anchored by the profile's own fields.
struct ZodiacToken {
    const char* token;
    SiderealMode mode;
};
inline constexpr ZodiacToken kZodiacTokens[] = {
    {"fagan-bradley", SiderealMode::FaganBradley},
    {"lahiri", SiderealMode::Lahiri},
    {"galcent-0sag", SiderealMode::GalacticCentre0Sag},
    {"true-citra", SiderealMode::TrueCitra},
    {"true-revati", SiderealMode::TrueRevati},
    {"true-pushya", SiderealMode::TruePushya},
    {"galcent-rgilbrand", SiderealMode::GalacticCentreGilBrand},
    {"galequ-iau1958", SiderealMode::GalacticEquatorIau1958},
    {"galequ-true", SiderealMode::GalacticEquatorTrue},
    {"galequ-mula", SiderealMode::GalacticEquatorMula},
    {"true-mula", SiderealMode::TrueMula},
    {"galcent-mula-wilhelm", SiderealMode::GalacticCentreMulaWilhelm},
    {"galcent-cochrane", SiderealMode::GalacticCentreCochrane},
};

// Whether a zodiac is defined at the instant (a star, the Galactic Centre or
// the galactic node held at a fixed longitude) rather than anchored at an
// epoch: such a zodiac has no anchor epoch, so no plane of the anchor
// (protocol v4 §3.5a; engine.hpp, SiderealMode).
constexpr bool defined_at_instant(SiderealMode m) {
    return m != SiderealMode::Tropical && m != SiderealMode::FaganBradley &&
           m != SiderealMode::Lahiri && m != SiderealMode::User;
}

} // namespace prometheia::server

#endif // PROMETHEIA_SERVER_ZODIACS_HPP
