#include "json_mapper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
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

/// Extract a scalar (string or number) value for a key from a JSON object substring.
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

/// Extract a nested object substring (including braces) for a given key.
std::string extractObject(const std::string& object, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = object.find(pattern);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const std::size_t colonPos = object.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::string();
    }
    const std::size_t braceStart = object.find_first_not_of(" \t\r\n", colonPos + 1);
    if (braceStart == std::string::npos || object[braceStart] != '{') {
        return std::string();
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = braceStart; i < object.size(); ++i) {
        const char ch = object[i];
        if (ch == '"' && (i == 0 || object[i - 1] != '\\')) {
            inString = !inString;
        }
        if (inString) {
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return object.substr(braceStart, i - braceStart + 1);
            }
        }
    }
    return std::string();
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

/// Join the string elements of a JSON string array into a single space-separated string.
std::string joinStringArray(const std::string& arrayBody) {
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t qs = arrayBody.find('"', pos);
        if (qs == std::string::npos) {
            break;
        }
        std::size_t qe = qs + 1;
        while (qe < arrayBody.size() && !(arrayBody[qe] == '"' && arrayBody[qe - 1] != '\\')) {
            ++qe;
        }
        if (qe >= arrayBody.size()) {
            break;
        }
        if (!out.empty()) {
            out += " ";
        }
        out += arrayBody.substr(qs + 1, qe - qs - 1);
        pos = qe + 1;
    }
    return out;
}

/// Parse a decimal amount string (e.g. "123.45" or "-12.3") into minor units (grosz/cents).
int64_t parseAmountToMinor(const std::string& value) {
    if (value.empty()) {
        return 0;
    }
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    double parsed = 0.0;
    if (!(stream >> parsed)) {
        return 0;
    }
    return static_cast<int64_t>(std::llround(parsed * 100.0));
}

my::currency parseCurrency(const std::string& value) {
    std::string upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (upper == "USD") {
        return my::currency::USD;
    }
    if (upper == "PLN") {
        return my::currency::PLN;
    }
    return my::currency::EUR;
}

/// Extract the amount + currency from a transaction/balance object, handling the
/// Enable Banking nested `{ "amount": "..", "currency": ".." }` structure as well
/// as flat legacy fields.
void extractAmount(const std::string& object, const std::string& nestedKey,
                   int64_t& outMinor, std::string& outCurrency) {
    const std::string nested = extractObject(object, nestedKey);
    if (!nested.empty()) {
        outMinor = parseAmountToMinor(extractStringValue(nested, "amount"));
        outCurrency = extractStringValue(nested, "currency");
        return;
    }
    outMinor = parseAmountToMinor(extractStringValue(object, "amount"));
    outCurrency = extractStringValue(object, "currency");
}
}  // namespace

std::vector<acc> parseAccounts(const std::string& json) {
    std::string body = findArrayBody(json, "accounts");
    if (body.empty()) {
        body = json;
    }
    const std::vector<std::string> objects = splitObjects(body);
    std::vector<acc> accounts;
    accounts.reserve(objects.size());
    for (const std::string& object : objects) {
        std::string name = extractStringValue(object, "name");
        if (name.empty()) name = extractStringValue(object, "product");
        if (name.empty()) name = extractStringValue(object, "displayName");
        if (name.empty()) name = extractStringValue(object, "accountName");
        if (name.empty()) name = extractStringValue(object, "uid");
        if (name.empty()) name = extractStringValue(object, "id");

        int64_t balanceMinor = 0;
        std::string currency;
        extractAmount(object, "balance_amount", balanceMinor, currency);
        if (balanceMinor == 0) {
            std::string balStr = extractStringValue(object, "balance");
            if (balStr.empty()) balStr = extractStringValue(object, "availableBalance");
            balanceMinor = parseAmountToMinor(balStr);
        }
        accounts.emplace_back(name.empty() ? "unknown-account" : name, balanceMinor);
    }
    return accounts;
}

std::vector<trans> parseTransactions(const std::string& json) {
    std::string body = findArrayBody(json, "transactions");
    if (body.empty()) {
        body = json;
    }
    const std::vector<std::string> objects = splitObjects(body);
    std::vector<trans> transactions;
    transactions.reserve(objects.size());
    for (const std::string& object : objects) {
        trans transaction{};

        // Counterparty objects (Enable Banking nests these under creditor/debtor).
        const std::string creditor = extractObject(object, "creditor");
        const std::string debtor = extractObject(object, "debtor");
        const std::string creditorName = !creditor.empty()
                ? extractStringValue(creditor, "name")
                : extractStringValue(object, "creditorName");
        const std::string debtorName = !debtor.empty()
                ? extractStringValue(debtor, "name")
                : extractStringValue(object, "debtorName");

        // Remittance information: snake_case array, then legacy fields.
        std::string remittance = joinStringArray(findArrayBody(object, "remittance_information"));
        if (remittance.empty()) remittance = extractStringValue(object, "remittanceInformationUnstructured");
        if (remittance.empty()) remittance = extractStringValue(object, "remittance_information_unstructured");

        // Amount + sign (credit_debit_indicator: CRDT = incoming, DBIT = outgoing).
        int64_t amountMinor = 0;
        std::string currency;
        extractAmount(object, "transaction_amount", amountMinor, currency);
        if (currency.empty()) {
            const std::string legacyNested = extractObject(object, "transactionAmount");
            if (!legacyNested.empty()) {
                amountMinor = parseAmountToMinor(extractStringValue(legacyNested, "amount"));
                currency = extractStringValue(legacyNested, "currency");
            }
        }
        std::string indicator = extractStringValue(object, "credit_debit_indicator");
        if (indicator.empty()) indicator = extractStringValue(object, "creditDebitIndicator");
        if (indicator.empty()) indicator = extractStringValue(object, "type");
        std::transform(indicator.begin(), indicator.end(), indicator.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        if ((indicator == "DBIT" || indicator == "EXPENSE") && amountMinor > 0) {
            amountMinor = -amountMinor;
        } else if ((indicator == "CRDT" || indicator == "INCOME") && amountMinor < 0) {
            amountMinor = -amountMinor;
        }

        // Name: prefer the "other" party, then remittance info.
        if (amountMinor < 0) {
            transaction.name = !creditorName.empty() ? creditorName : debtorName;
        } else {
            transaction.name = !debtorName.empty() ? debtorName : creditorName;
        }
        if (transaction.name.empty()) transaction.name = remittance;
        if (transaction.name.empty()) transaction.name = extractStringValue(object, "name");

        // Date: Enable Banking uses booking_date / value_date / transaction_date.
        transaction.date = extractStringValue(object, "booking_date");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "value_date");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "transaction_date");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "bookingDate");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "valueDate");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "transactionDate");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "date");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "timestamp");
        if (transaction.date.empty()) transaction.date = extractStringValue(object, "created_at");
        // Trim any time component (keep YYYY-MM-DD).
        if (transaction.date.size() > 10) {
            transaction.date = transaction.date.substr(0, 10);
        }

        transaction.opis = remittance;
        if (transaction.opis.empty()) transaction.opis = extractStringValue(object, "description");

        transaction.amount = amountMinor;
        transaction.currencyCode = currency;
        transaction.curr = parseCurrency(currency);

        if (amountMinor < 0) {
            transaction.type = my::type::expense;
        } else if (amountMinor > 0) {
            transaction.type = my::type::income;
        } else {
            transaction.type = my::type::expense;
        }

        transaction.from = debtorName;
        transaction.to = creditorName;
        transaction.category = my::category::other;
        transaction.tag = my::tag::opt;

        // Real bank transaction id for de-duplication.
        transaction.bankTxId = extractStringValue(object, "entry_reference");
        if (transaction.bankTxId.empty()) transaction.bankTxId = extractStringValue(object, "transaction_id");
        if (transaction.bankTxId.empty()) transaction.bankTxId = extractStringValue(object, "entryReference");
        if (transaction.bankTxId.empty()) transaction.bankTxId = extractStringValue(object, "transactionId");

        transactions.push_back(transaction);
    }
    return transactions;
}

BankBalance parseBalance(const std::string& json) {
    BankBalance result;
    std::string body = findArrayBody(json, "balances");
    std::string object;
    if (!body.empty()) {
        const std::vector<std::string> objects = splitObjects(body);
        if (!objects.empty()) {
            // Prefer a "closing booked" (CLBD) balance if present, else the first.
            object = objects.front();
            for (const std::string& candidate : objects) {
                const std::string type = extractStringValue(candidate, "balance_type");
                if (type == "CLBD" || type == "closingBooked") {
                    object = candidate;
                    break;
                }
            }
        }
    }
    if (object.empty()) {
        object = json;
    }
    int64_t minor = 0;
    std::string currency;
    extractAmount(object, "balance_amount", minor, currency);
    if (currency.empty() && minor == 0) {
        const std::string nested = extractObject(object, "balanceAmount");
        if (!nested.empty()) {
            minor = parseAmountToMinor(extractStringValue(nested, "amount"));
            currency = extractStringValue(nested, "currency");
        }
    }
    result.minorUnits = minor;
    result.currency = currency;
    result.found = !object.empty();
    return result;
}

BankAccountDetails parseAccountDetails(const std::string& json) {
    BankAccountDetails details;
    details.name = extractStringValue(json, "name");
    if (details.name.empty()) details.name = extractStringValue(json, "product");
    details.currency = extractStringValue(json, "currency");

    const std::string accountId = extractObject(json, "account_id");
    if (!accountId.empty()) {
        details.iban = extractStringValue(accountId, "iban");
    }
    if (details.iban.empty()) {
        details.iban = extractStringValue(json, "iban");
    }
    return details;
}
