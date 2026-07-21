#include "acc.h"

#include <algorithm>
#include <utility>

acc::acc(std::string name, int64_t balance) : name(std::move(name)), balance(balance) {}

std::string acc::getName() const {
    return name;
}

int64_t acc::getBalance() const {
    return balance;
}

const std::vector<trans>& acc::getTransactions() const {
    return transactions;
}

void acc::addTransaction(const trans& transaction) {
    transactions.push_back(transaction);
}

void acc::deleteTransaction(const std::string& bankTxId) {
    // Match on the unique bank transaction id so we never delete unrelated
    // transactions that merely share a display name.
    transactions.erase(
            std::remove_if(transactions.begin(), transactions.end(), [&](const trans& transaction) {
                return !bankTxId.empty() && transaction.bankTxId == bankTxId;
            }),
            transactions.end());
}

void acc::setBalance(int64_t newBalance) {
    balance = newBalance;
}
