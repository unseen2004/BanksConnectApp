#ifndef BANKSCONNECTAPP_ACC_H
#define BANKSCONNECTAPP_ACC_H

#include <string>
#include <vector>

#include "trans.h"

class acc {
    std::string name;
    int balance;
    std::vector<trans> transactions;

public:
    acc(std::string name, int balance);
    std::string getName() const;
    int getBalance() const;
    const std::vector<trans>& getTransactions() const;
    void addTransaction(const trans& transaction);
    void deleteTransaction(const std::string& transactionName);
    void setBalance(int newBalance);
};

#endif //BANKSCONNECTAPP_ACC_H
