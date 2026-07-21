#ifndef BANKSCONNECTAPP_JSON_MAPPER_H
#define BANKSCONNECTAPP_JSON_MAPPER_H

#include <cstdint>
#include <string>
#include <vector>

#include "acc.h"

/// A parsed account balance from an Enable Banking `/balances` response.
struct BankBalance {
    int64_t minorUnits = 0;   // amount in minor units (grosz/cents)
    std::string currency;     // ISO currency code (may be empty)
    bool found = false;       // whether a balance was actually parsed
};

/// A parsed account resource from an Enable Banking `/details` response.
struct BankAccountDetails {
    std::string name;      // human-friendly name/product (may be empty)
    std::string currency;  // ISO currency code (may be empty)
    std::string iban;      // IBAN if available (may be empty)
};

std::vector<acc> parseAccounts(const std::string& json);
std::vector<trans> parseTransactions(const std::string& json);

/// Parse the first balance entry from an Enable Banking balances response.
BankBalance parseBalance(const std::string& json);

/// Parse an Enable Banking account details response.
BankAccountDetails parseAccountDetails(const std::string& json);

#endif //BANKSCONNECTAPP_JSON_MAPPER_H
