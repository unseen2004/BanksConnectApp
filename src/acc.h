#ifndef BANKSCONNECTAPP_ACC_H
#define BANKSCONNECTAPP_ACC_H

#include <cstdint>
#include <string>
#include <vector>

#include "trans.h"

class acc {
    std::string name;
    int64_t balance;
    std::vector<trans> transactions;

public:
    acc(std::string name, int64_t balance);
    std::string getName() const;
    int64_t getBalance() const;
    const std::vector<trans>& getTransactions() const;
    void addTransaction(const trans& transaction);
    // Delete a transaction by its unique bank transaction id (names are not unique).
    void deleteTransaction(const std::string& bankTxId);
    void setBalance(int64_t newBalance);
};

#endif //BANKSCONNECTAPP_ACC_H
