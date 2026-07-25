#ifndef BANKSCONNECTAPP_MONEY_H
#define BANKSCONNECTAPP_MONEY_H

#include <cstdint>
#include <string>

namespace money {

/// Parses a decimal amount into minor units (grosz / cents) without going through
/// a binary floating point value.
///
/// Amounts such as "8.15" are not exactly representable as a double, so
/// `llround(value * 100)` can land a grosz either side of the true figure. Over a
/// long statement those errors accumulate and the computed balance drifts away
/// from the bank's, which is why the digits are accumulated as integers here.
///
/// Accepted: optional sign, digits, and an optional fractional part separated by
/// '.' or ',' (only when the string contains no '.'). Internal spaces are ignored
/// so "1 234.56" parses. More than two fractional digits are rounded half away
/// from zero. Returns false for anything else, including empty input and values
/// that would overflow int64_t.
bool parseToMinor(const std::string& text, int64_t& outMinor);

/// Convenience wrapper for callers that treat unparsable input as zero.
int64_t parseToMinorOrZero(const std::string& text);

/// Renders minor units back as a plain decimal string, e.g. -815 -> "-8.15".
std::string formatMinor(int64_t minor);

}  // namespace money

#endif  // BANKSCONNECTAPP_MONEY_H
