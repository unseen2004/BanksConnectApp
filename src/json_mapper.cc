#include "json_mapper.h"
#include <locale>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>

namespace {
std::string trim(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) {
        return std::string();
    }
    return std::string(begin, end);
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string extractStringValue(const std::string& object, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = object.find(pattern);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const std::size_t colonPos = object.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::string();
    }
    std::size_t valueStart = object.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return std::string();
    }
    if (object[valueStart] == '"') {
        const std::size_t valueEnd = object.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) {
            return std::string();
        }
        return object.substr(valueStart + 1, valueEnd - valueStart - 1);
    }
    const std::size_t valueEnd = object.find_first_of(",}\r\n", valueStart);
    return trim(object.substr(valueStart, valueEnd - valueStart));
}

int extractIntValue(const std::string& object, const std::string& key, int fallback) {
    const std::string value = extractStringValue(object, key);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    if (parsed > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    if (parsed < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(parsed);
}

std::vector<std::string> splitObjects(const std::string& jsonArray) {
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < jsonArray.size(); ++i) {
        const char ch = jsonArray[i];
        if (ch == '"' && (i == 0 || jsonArray[i - 1] != '\\')) {
            inString = !inString;
        }
        if (inString) {
            continue;
        }
        if (ch == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                objects.push_back(jsonArray.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return objects;
}

std::string findArrayBody(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const std::size_t bracketStart = json.find('[', keyPos + pattern.size());
    if (bracketStart == std::string::npos) {
        return std::string();
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = bracketStart; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '"' && (i == 0 || json[i - 1] != '\\')) {
            inString = !inString;
        }
        if (inString) {
            continue;
        }
        if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                return json.substr(bracketStart + 1, i - bracketStart - 1);
            }
        }
    }
    return std::string();
}

my::currency parseCurrency(const std::string& value) {
    const std::string upper = [&]() {
        std::string copy = value;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return copy;
    }();
    if (upper == "USD") {
        return my::currency::USD;
    }
    if (upper == "PLN") {
        return my::currency::PLN;
    }
    return my::currency::EUR;
}

my::type parseTransactionType(const std::string& value) {
    const std::string lower = [&]() {
        std::string copy = value;
        std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return copy;
    }();
    if (lower.find("income") != std::string::npos || lower.find("credit") != std::string::npos) {
        return my::type::income;
    }
    if (lower.find("inside") != std::string::npos || lower.find("transfer") != std::string::npos) {
        return my::type::inside;
    }
    return my::type::expense;
}
}

std::vector<acc> parseAccounts(const std::string& json) {
    const std::string body = !findArrayBody(json, "accounts").empty() ? findArrayBody(json, "accounts") : json;
    const std::vector<std::string> objects = splitObjects(body);
    std::vector<acc> accounts;
    accounts.reserve(objects.size());
    for (const std::string& object : objects) {
        std::string name = extractStringValue(object, "name");
        if (name.empty()) {
            name = extractStringValue(object, "displayName");
        }
        if (name.empty()) {
            name = extractStringValue(object, "accountName");
        }
        if (name.empty()) {
            name = extractStringValue(object, "id");
        }
        std::string balStr = extractStringValue(object, "balance");
        if (balStr.empty()) balStr = extractStringValue(object, "availableBalance");
        int balanceGrosz = 0;
        try {
            std::istringstream iss(balStr);
            iss.imbue(std::locale::classic());
            double d = 0;
            if (iss >> d) balanceGrosz = static_cast<int>(d * 100);
        } catch (...) {}
        accounts.emplace_back(name.empty() ? "unknown-account" : name, balanceGrosz);
    }
    return accounts;
}

std::vector<trans> parseTransactions(const std::string& json) {
    const std::string body = !findArrayBody(json, "transactions").empty() ? findArrayBody(json, "transactions") : json;
    const std::vector<std::string> objects = splitObjects(body);
    std::vector<trans> transactions;
    transactions.reserve(objects.size());
    for (const std::string& object : objects) {
        trans transaction{};
        
        // Name parsing
        transaction.name = extractStringValue(object, "creditorName");
        if (transaction.name.empty()) transaction.name = extractStringValue(object, "debtorName");
        if (transaction.name.empty()) transaction.name = extractStringValue(object, "remittanceInformationUnstructured");
        if (transaction.name.empty()) transaction.name = extractStringValue(object, "name");
        
        // Date parsing
        transaction.date = extractStringValue(object, "bookingDate");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "valueDate");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "date");

        // Description parsing
        transaction.opis = extractStringValue(object, "remittanceInformationUnstructured");
        if (transaction.opis.empty()) transaction.opis = extractStringValue(object, "description");
        
        // Amount parsing (handles negative and decimal values properly)
        std::string amountStr = extractStringValue(object, "amount");
        double parsedAmount = 0.0;
        if (!amountStr.empty()) {
            std::istringstream stream(amountStr);
            stream.imbue(std::locale::classic());
            stream >> parsedAmount;
        }
        transaction.amount = static_cast<int>(parsedAmount * 100.0);
        
        // Type inference based on amount
        if (transaction.amount < 0) {
            transaction.type = my::type::expense;
        } else if (transaction.amount > 0) {
            transaction.type = my::type::income;
        } else {
            transaction.type = my::type::expense;
        }
        
        transaction.curr = parseCurrency(extractStringValue(object, "currency"));
        transaction.from = extractStringValue(object, "debtorName");
        transaction.to = extractStringValue(object, "creditorName");
        transaction.category = my::category::other;
        transaction.tag = my::tag::opt;
        
        transactions.push_back(transaction);
    }
    return transactions;
}
