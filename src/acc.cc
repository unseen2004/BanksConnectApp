//
// Created by maks on 7/8/26.
//

#include "acc.h"

acc::acc(std::string name, int balance) : name(name), balance(balance) {}

std::string acc::getName() const {
    return name;
}

int acc::getBalance() const {
    return balance;
}

void acc::setBalance(int newBalance) {
    balance = newBalance;
}
