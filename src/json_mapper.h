#ifndef BANKSCONNECTAPP_JSON_MAPPER_H
#define BANKSCONNECTAPP_JSON_MAPPER_H

#include <string>
#include <vector>

#include "acc.h"

std::vector<acc> parseAccounts(const std::string& json);
std::vector<trans> parseTransactions(const std::string& json);

#endif //BANKSCONNECTAPP_JSON_MAPPER_H
