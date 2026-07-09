#include "acc.h"

#include <algorithm>
#include <utility>

acc::acc(std::string name, int balance) : name(std::move(name)), balance(balance) {}

std::string acc::getName() const {
    return name;
}

int acc::getBalance() const {
    return balance;
}

const std::vector<trans>& acc::getTransactions() const {
    return transactions;
}

void acc::addTransaction(const trans& transaction) {
    transactions.push_back(transaction);
}

void acc::deleteTransaction(const std::string& transactionName) {
    transactions.erase(
            std::remove_if(transactions.begin(), transactions.end(), [&](const trans& transaction) {
                return transaction.name == transactionName;
            }),
            transactions.end());
}

void acc::setBalance(int newBalance) {
    balance = newBalance;
}
