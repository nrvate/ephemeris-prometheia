// SPDX-License-Identifier: GPL-2.0-or-later
//
// Named hypothetical bodies: element files in JSON Lines, one body per line.
// The format is Prometheia's own and mirrors ephemeris protocol v4's kind-4
// fields; docs/HYPOTHETICALS.md, "Element files", is its definition.
#ifndef PROMETHEIA_HYPOTHETICALS_HPP
#define PROMETHEIA_HYPOTHETICALS_HPP

#include <string>
#include <string_view>
#include <vector>

#include <prometheia/engine.hpp>
#include <prometheia/error.hpp>

namespace prometheia::hypotheticals {

struct Body {
    std::string token;    // what a request names it by, lowercase
    std::string name;     // display name; empty when the file gives none
    std::string set;      // the element set's short name (answers carry it)
    std::string citation; // where the numbers come from
    PolynomialElements elements;
};

// Parses element-file text. `origin` names the text in error messages, which
// point at a line: "cupido.jsonl:3: ...". Every line is checked strictly --
// an unknown field is an error, not an omission, because a mistyped field
// name would otherwise silently leave an element at zero.
Result<std::vector<Body>> parse(std::string_view text, std::string_view origin);

} // namespace prometheia::hypotheticals

#endif // PROMETHEIA_HYPOTHETICALS_HPP
