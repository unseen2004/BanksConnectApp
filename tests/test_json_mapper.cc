#include "test_support.h"

#include "json_mapper.h"

TEST(parsesEnableBankingTransactionsWithSnakeCaseFields) {
    const std::string json = R"({"transactions":[
        {"transaction_amount":{"amount":"49.99","currency":"PLN"},
         "credit_debit_indicator":"DBIT",
         "creditor":{"name":"Zabka"},
         "booking_date":"2026-07-01",
         "entry_reference":"ref-1",
         "remittance_information":["Zakupy","spozywcze"]},
        {"transaction_amount":{"amount":"3500.00","currency":"PLN"},
         "credit_debit_indicator":"CRDT",
         "debtor":{"name":"Pracodawca"},
         "booking_date":"2026-07-02",
         "entry_reference":"ref-2"}
    ]})";

    const std::vector<trans> transactions = parseTransactions(json);
    EXPECT_EQ(transactions.size(), std::size_t(2));

    EXPECT_EQ(transactions[0].amount, -4999);
    EXPECT_EQ(transactions[0].name, std::string("Zabka"));
    EXPECT_EQ(transactions[0].date, std::string("2026-07-01"));
    EXPECT_EQ(transactions[0].bankTxId, std::string("ref-1"));
    EXPECT_EQ(transactions[0].opis, std::string("Zakupy spozywcze"));
    EXPECT_TRUE(transactions[0].type == my::type::expense);

    EXPECT_EQ(transactions[1].amount, 350000);
    EXPECT_EQ(transactions[1].name, std::string("Pracodawca"));
    EXPECT_TRUE(transactions[1].type == my::type::income);
}

TEST(parsesLegacyCamelCaseTransactionFields) {
    const std::string json = R"({"transactions":[
        {"transactionAmount":{"amount":"12.50","currency":"EUR"},
         "creditDebitIndicator":"DBIT",
         "creditorName":"Shop",
         "bookingDate":"2026-06-30",
         "entryReference":"legacy-1",
         "remittanceInformationUnstructured":"Card payment"}
    ]})";

    const std::vector<trans> transactions = parseTransactions(json);
    EXPECT_EQ(transactions.size(), std::size_t(1));
    EXPECT_EQ(transactions[0].amount, -1250);
    EXPECT_EQ(transactions[0].currencyCode, std::string("EUR"));
    EXPECT_EQ(transactions[0].bankTxId, std::string("legacy-1"));
}

TEST(debitIndicatorMakesTheAmountNegativeExactlyOnce) {
    // A provider that already signs the amount must not have it flipped again.
    const std::string json = R"({"transactions":[
        {"transaction_amount":{"amount":"-20.00","currency":"PLN"},
         "credit_debit_indicator":"DBIT","entry_reference":"signed"}
    ]})";
    const std::vector<trans> transactions = parseTransactions(json);
    EXPECT_EQ(transactions.size(), std::size_t(1));
    EXPECT_EQ(transactions[0].amount, -2000);
}

TEST(amountsAreParsedExactlyRatherThanViaDouble) {
    const std::string json = R"({"transactions":[
        {"transaction_amount":{"amount":"8.15","currency":"PLN"},
         "credit_debit_indicator":"DBIT","entry_reference":"a"},
        {"transaction_amount":{"amount":"70.07","currency":"PLN"},
         "credit_debit_indicator":"DBIT","entry_reference":"b"}
    ]})";
    const std::vector<trans> transactions = parseTransactions(json);
    EXPECT_EQ(transactions[0].amount, -815);
    EXPECT_EQ(transactions[1].amount, -7007);
}

TEST(prefersTheClosingBookedBalance) {
    const std::string json = R"({"balances":[
        {"balance_type":"ITAV","balance_amount":{"amount":"100.00","currency":"PLN"}},
        {"balance_type":"CLBD","balance_amount":{"amount":"250.75","currency":"PLN"}}
    ]})";
    const BankBalance balance = parseBalance(json);
    EXPECT_TRUE(balance.found);
    EXPECT_EQ(balance.minorUnits, 25075);
    EXPECT_EQ(balance.currency, std::string("PLN"));
}

TEST(parsesAccountDetailsIncludingNestedIban) {
    const std::string json = R"({"name":"Konto osobiste","currency":"PLN",
        "account_id":{"iban":"PL61109010140000071219812874"}})";
    const BankAccountDetails details = parseAccountDetails(json);
    EXPECT_EQ(details.name, std::string("Konto osobiste"));
    EXPECT_EQ(details.currency, std::string("PLN"));
    EXPECT_EQ(details.iban, std::string("PL61109010140000071219812874"));
}

TEST(handlesEmptyAndMalformedPayloadsWithoutThrowing) {
    EXPECT_EQ(parseTransactions("").size(), std::size_t(0));
    EXPECT_EQ(parseTransactions("{}").size(), std::size_t(0));
    EXPECT_EQ(parseTransactions(R"({"transactions":[]})").size(), std::size_t(0));
    EXPECT_EQ(parseAccounts("").size(), std::size_t(0));
}

TEST(parsesAccountsAndBalances) {
    const std::string json = R"({"accounts":[
        {"uid":"acc-1","name":"Konto","balance_amount":{"amount":"1234.56","currency":"PLN"}}
    ]})";
    const std::vector<acc> accounts = parseAccounts(json);
    EXPECT_EQ(accounts.size(), std::size_t(1));
    EXPECT_EQ(accounts[0].getName(), std::string("Konto"));
    EXPECT_EQ(accounts[0].getBalance(), 123456);
}
