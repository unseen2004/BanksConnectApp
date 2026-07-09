#ifndef BANKSCONNECTAPP_ACC_INVEST_H
#define BANKSCONNECTAPP_ACC_INVEST_H

#include <string>
#include <vector>

#include "acc.h"

struct investAsset {
    std::string name;
    int amount_start;
    int amount_end;
    std::string date_start;
    std::string date_end;
};

class accInvest : public acc {
    std::vector<investAsset> investments;

public:
    accInvest(std::string name, int balance);
    void addInvestment(const investAsset& investment);
    void deleteInvestment(const std::string& investmentName);
    const std::vector<investAsset>& getInvestments() const;
};

#endif //BANKSCONNECTAPP_ACC_INVEST_H
