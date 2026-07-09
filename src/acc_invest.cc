#include "acc_invest.h"

#include <algorithm>
#include <utility>

accInvest::accInvest(std::string name, int balance) : acc(std::move(name), balance) {}

void accInvest::addInvestment(const investAsset& investment) {
    investments.push_back(investment);
}

void accInvest::deleteInvestment(const std::string& investmentName) {
    investments.erase(
            std::remove_if(investments.begin(), investments.end(), [&](const investAsset& investment) {
                return investment.name == investmentName;
            }),
            investments.end());
}

const std::vector<investAsset>& accInvest::getInvestments() const {
    return investments;
}
