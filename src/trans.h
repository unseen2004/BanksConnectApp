//
// Created by maks on 7/8/26.
//

#ifndef BANKSCONNECTAPP_TRANS_H
#define BANKSCONNECTAPP_TRANS_H
#include <string>
#include <vector>

namespace my {

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
    int amount;
    my::currency curr;
    std::string from;
    std::string to;
    my::type type;
    my::category category;
    std::string date;
    my::tag tag;
    std::vector<trans> subtransactions;
};

struct nextMonth {
    my::month month;
    std::hash_map<my::category, int> categorySumsCel;
    std::hash_map<my::category, int> categorySumsreal;
    std::hash_map<my::category, std::vector<trans>> categorySumsSub;
};

struct toDoComming {
    std::string name;
    std::string opis;
    int amount;
    std::string to;
};

#endif //BANKSCONNECTAPP_TRANS_H
