#include "money.h"

#include <cctype>
#include <cstdlib>
#include <limits>

namespace money {
namespace {
constexpr int kMinorDigits = 2;
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

bool mulAdd(int64_t& acc, int64_t digit) {
    if (acc > (kMax - digit) / 10) {
        return false;
    }
    acc = acc * 10 + digit;
    return true;
}
}  // namespace

bool parseToMinor(const std::string& text, int64_t& outMinor) {
    // When a '.' is present it is the decimal separator, which makes any ',' a
    // thousands separator ("1,234.56").
    const bool commaIsGrouping = text.find('.') != std::string::npos;

    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char ch : text) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '_' || ch == '\'') {
            continue;
        }
        if (ch == ',' && commaIsGrouping) {
            continue;
        }
        cleaned.push_back(ch);
    }
    if (cleaned.empty()) {
        return false;
    }

    std::size_t index = 0;
    bool negative = false;
    if (cleaned[index] == '+' || cleaned[index] == '-') {
        negative = cleaned[index] == '-';
        ++index;
    }

    const char separator = commaIsGrouping ? '.' : ',';

    int64_t units = 0;
    bool sawIntegerDigit = false;
    while (index < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[index]))) {
        if (!mulAdd(units, cleaned[index] - '0')) {
            return false;
        }
        sawIntegerDigit = true;
        ++index;
    }

    int64_t fraction = 0;
    bool sawFractionDigit = false;
    int fractionDigits = 0;
    bool roundUp = false;
    if (index < cleaned.size() && cleaned[index] == separator) {
        ++index;
        while (index < cleaned.size() && std::isdigit(static_cast<unsigned char>(cleaned[index]))) {
            const int digit = cleaned[index] - '0';
            if (fractionDigits < kMinorDigits) {
                fraction = fraction * 10 + digit;
                ++fractionDigits;
            } else if (fractionDigits == kMinorDigits) {
                // First discarded digit decides the rounding direction.
                roundUp = digit >= 5;
                ++fractionDigits;
            }
            sawFractionDigit = true;
            ++index;
        }
    }

    if (index != cleaned.size() || (!sawIntegerDigit && !sawFractionDigit)) {
        return false;
    }

    while (fractionDigits < kMinorDigits) {
        fraction *= 10;
        ++fractionDigits;
    }

    int64_t minor = 0;
    if (units > kMax / 100) {
        return false;
    }
    minor = units * 100;
    if (minor > kMax - fraction) {
        return false;
    }
    minor += fraction;
    if (roundUp) {
        if (minor == kMax) {
            return false;
        }
        ++minor;
    }

    outMinor = negative ? -minor : minor;
    return true;
}

int64_t parseToMinorOrZero(const std::string& text) {
    int64_t minor = 0;
    return parseToMinor(text, minor) ? minor : 0;
}

std::string formatMinor(int64_t minor) {
    const bool negative = minor < 0;
    // Negating INT64_MIN is undefined, so work with the unsigned magnitude.
    const uint64_t magnitude = negative ? (~static_cast<uint64_t>(minor) + 1ULL)
                                        : static_cast<uint64_t>(minor);
    const uint64_t units = magnitude / 100;
    const uint64_t cents = magnitude % 100;

    std::string out;
    if (negative) {
        out.push_back('-');
    }
    out += std::to_string(units);
    out.push_back('.');
    if (cents < 10) {
        out.push_back('0');
    }
    out += std::to_string(cents);
    return out;
}

}  // namespace money
