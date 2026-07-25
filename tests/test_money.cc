#include "test_support.h"

#include "money.h"

#include <cstdint>
#include <limits>

namespace {
int64_t parsed(const std::string& text) {
    int64_t minor = 0;
    money::parseToMinor(text, minor);
    return minor;
}
}  // namespace

TEST(parsesWholeAndFractionalAmounts) {
    EXPECT_EQ(parsed("0"), 0);
    EXPECT_EQ(parsed("1"), 100);
    EXPECT_EQ(parsed("123.45"), 12345);
    EXPECT_EQ(parsed("-12.3"), -1230);
    EXPECT_EQ(parsed("0.07"), 7);
    EXPECT_EQ(parsed(".5"), 50);
    EXPECT_EQ(parsed("+3.00"), 300);
}

TEST(parsesAmountsThatAreNotExactInBinaryFloatingPoint) {
    // These are the cases the old llround(value * 100.0) implementation could get
    // wrong by a grosz.
    EXPECT_EQ(parsed("8.15"), 815);
    EXPECT_EQ(parsed("1.005"), 101);
    EXPECT_EQ(parsed("2.675"), 268);
    EXPECT_EQ(parsed("70.07"), 7007);
    EXPECT_EQ(parsed("4.35"), 435);
}

TEST(roundsExtraFractionalDigitsHalfAwayFromZero) {
    EXPECT_EQ(parsed("1.234"), 123);
    EXPECT_EQ(parsed("1.235"), 124);
    EXPECT_EQ(parsed("1.999"), 200);
    EXPECT_EQ(parsed("-1.235"), -124);
    EXPECT_EQ(parsed("1.2349999"), 123);
}

TEST(ignoresGroupingWhitespaceAndAcceptsCommaWhenUnambiguous) {
    EXPECT_EQ(parsed("1 234.56"), 123456);
    EXPECT_EQ(parsed("12,34"), 1234);
    EXPECT_EQ(parsed("1,234.56"), 123456);
}

TEST(rejectsMalformedInput) {
    int64_t minor = 999;
    EXPECT_FALSE(money::parseToMinor("", minor));
    EXPECT_FALSE(money::parseToMinor("abc", minor));
    EXPECT_FALSE(money::parseToMinor("12.34USD", minor));
    EXPECT_FALSE(money::parseToMinor("1.2.3", minor));
    EXPECT_FALSE(money::parseToMinor("-", minor));
    EXPECT_FALSE(money::parseToMinor("1e5", minor));
    // The out-parameter is left untouched when parsing fails.
    EXPECT_EQ(minor, 999);
}

TEST(rejectsValuesThatWouldOverflow) {
    int64_t minor = 0;
    EXPECT_FALSE(money::parseToMinor("99999999999999999999", minor));
    EXPECT_TRUE(money::parseToMinor("92233720368547757.00", minor));
    EXPECT_FALSE(money::parseToMinor("92233720368547759.00", minor));
}

TEST(unparsableAmountsFallBackToZeroForCallersThatWantThat) {
    EXPECT_EQ(money::parseToMinorOrZero("nonsense"), 0);
    EXPECT_EQ(money::parseToMinorOrZero("5.5"), 550);
}

TEST(formatsMinorUnitsBackToDecimalText) {
    EXPECT_EQ(money::formatMinor(0), std::string("0.00"));
    EXPECT_EQ(money::formatMinor(7), std::string("0.07"));
    EXPECT_EQ(money::formatMinor(12345), std::string("123.45"));
    EXPECT_EQ(money::formatMinor(-815), std::string("-8.15"));
    EXPECT_EQ(money::formatMinor(-5), std::string("-0.05"));
}

TEST(formatMinorHandlesTheMostNegativeValueWithoutUndefinedBehaviour) {
    const int64_t lowest = std::numeric_limits<int64_t>::min();
    EXPECT_EQ(money::formatMinor(lowest), std::string("-92233720368547758.08"));
}

TEST(parsingAndFormattingRoundTrip) {
    const char* samples[] = {"0.00", "0.01", "-0.01", "1.00", "8.15", "1234.56", "-99.99"};
    for (const char* sample : samples) {
        int64_t minor = 0;
        EXPECT_TRUE(money::parseToMinor(sample, minor));
        EXPECT_EQ(money::formatMinor(minor), std::string(sample));
    }
}
