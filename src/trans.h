#ifndef BANKSCONNECTAPP_TRANS_H
#define BANKSCONNECTAPP_TRANS_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace my {

enum class month {
    january,
    february,
    march,
    april,
    may,
    june,
    july,
    august,
    september,
    october,
    november,
    december
};

enum class tag {
    must,
    opt,
    waste,
};

enum class currency {
    USD,
    EUR,
    PLN,
};

enum class type {
    income,
    expense,
    inside,
};

enum class category {
    food,
    transport,
    entertainment,
    utilities,
    alko,
    wyjazdy,
    other
};

}

struct trans {
    std::string name;
    std::string opis;
    int64_t amount = 0;        // in minor currency units (e.g. grosz)
    my::currency curr = my::currency::EUR;
    std::string currencyCode;  // raw ISO currency from the bank (e.g. "PLN", "EUR")
    std::string from;
    std::string to;
    my::type type = my::type::expense;
    my::category category = my::category::other;
    std::string date;
    my::tag tag = my::tag::opt;
    std::string bankTxId;      // real bank transaction id / entry reference (for de-duplication)
    std::vector<trans> subtransactions;
};

struct nextMonth {
    my::month month;
    std::unordered_map<my::category, int> categorySumsCel;
    std::unordered_map<my::category, int> categorySumsreal;
    std::unordered_map<my::category, std::vector<trans>> categorySumsSub;
};

struct toDoComming {
    std::string name;
    std::string opis;
    int amount;
    std::string to;
};

#endif //BANKSCONNECTAPP_TRANS_H
