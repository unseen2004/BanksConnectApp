//
// Created by maks on 7/8/26.
//

#ifndef BANKSCONNECTAPP_ACC_INVEST_H
#define BANKSCONNECTAPP_ACC_INVEST_H


struct investAssest {
    std::string name;
    int amount_start;
    int amount_end; // before getting out is 0
    std::string date_start;
    std::string date_end;
};

class accInvest : acc {
    std::vector<investAssest> investments;
public:
    accInvest(std::string name, int balance);
    void addInvestment(const investAssest& investment);
    void deleteInvestment(const std::string& investmentName);
    const std::vector<investAssest>& getInvestments() const;
};


#endif //BANKSCONNECTAPP_ACC_INVEST_H
