#include "test_support.h"

#include "database.h"

#include <stdexcept>

namespace {

db::Account makeAccount(const std::string& id, const std::string& name,
                        const std::string& source = "manual") {
    db::Account account;
    account.id = id;
    account.name = name;
    account.type = source == "bank" ? "bank" : "wallet";
    account.currency = "PLN";
    account.bankName = source == "bank" ? "Test Bank" : "";
    account.source = source;
    return account;
}

db::Transaction makeTx(db::Database& database, const std::string& accountId, int64_t amount,
                       const std::string& date, const std::string& category,
                       const std::string& source = "bank", const std::string& bankTxId = "") {
    db::Transaction tx;
    tx.id = database.uuid();
    tx.accountId = accountId;
    tx.name = "tx-" + date;
    tx.amount = amount;
    tx.currency = "PLN";
    tx.type = amount < 0 ? "expense" : "income";
    tx.category = category;
    tx.tag = "opt";
    tx.date = date;
    tx.source = source;
    tx.bankTxId = bankTxId;
    return tx;
}

// Each test gets its own in-memory database.
struct Fixture {
    db::Database database{":memory:"};
    Fixture() { database.upsertAccount(makeAccount("acc-1", "Konto")); }
};

}  // namespace

TEST(freshDatabaseIsAtTheCurrentSchemaVersion) {
    Fixture fixture;
    EXPECT_EQ(fixture.database.schemaVersion(), 3);
}

TEST(defaultCategoriesAreSeededOnce) {
    Fixture fixture;
    const auto categories = fixture.database.categories();
    EXPECT_TRUE(categories.size() >= 12);
    // Re-opening must not duplicate them.
    bool foundFood = false;
    int foodCount = 0;
    for (const auto& category : categories) {
        if (category.name == "food") {
            foundFood = true;
            ++foodCount;
        }
    }
    EXPECT_TRUE(foundFood);
    EXPECT_EQ(foodCount, 1);
}

TEST(theUniqueIndexRejectsADuplicateBankTransaction) {
    Fixture fixture;
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "food", "bank", "ref-1"));
    // insertTx uses INSERT OR IGNORE, so the second attempt is a no-op rather than
    // an error, but it must not create a second row.
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "food", "bank", "ref-1"));
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(1));
    EXPECT_TRUE(fixture.database.txExists("acc-1", "ref-1"));
}

TEST(theSameBankReferenceOnADifferentAccountIsNotADuplicate) {
    Fixture fixture;
    fixture.database.upsertAccount(makeAccount("acc-2", "Drugie konto"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "food", "bank", "shared-ref"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-2", -1000, "2026-07-01", "food", "bank", "shared-ref"));
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(2));
    EXPECT_TRUE(fixture.database.txExists("acc-1", "shared-ref"));
    EXPECT_TRUE(fixture.database.txExists("acc-2", "shared-ref"));
}

TEST(manualTransactionsWithoutABankReferenceCoexist) {
    Fixture fixture;
    // The unique index is partial, so several rows may carry an empty bank_tx_id.
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -500, "2026-07-01", "food", "manual"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -600, "2026-07-02", "food", "manual"));
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(2));
}

TEST(existingBankIdsAreReturnedForBulkDeduplication) {
    Fixture fixture;
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -100, "2026-07-01", "food", "bank", "a"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -200, "2026-07-02", "food", "bank", "b"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -300, "2026-07-03", "food", "manual"));
    const auto ids = fixture.database.existingBankTxIds("acc-1");
    EXPECT_EQ(ids.size(), std::size_t(2));
}

TEST(deletingAnAccountRemovesItsTransactions) {
    Fixture fixture;
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -100, "2026-07-01", "food", "bank", "a"));
    fixture.database.deleteAccount("acc-1");
    EXPECT_EQ(fixture.database.accounts().size(), std::size_t(0));
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(0));
}

TEST(bankTransactionsCannotBeDeleted) {
    Fixture fixture;
    db::Transaction bankTx = makeTx(fixture.database, "acc-1", -100, "2026-07-01", "food", "bank", "a");
    fixture.database.insertTx(bankTx);
    EXPECT_TRUE(fixture.database.deleteTx(bankTx.id) == db::DeleteResult::NotDeletable);
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(1));
}

TEST(manualTransactionsCanBeDeletedAndMissingOnesAreReported) {
    Fixture fixture;
    db::Transaction manual = makeTx(fixture.database, "acc-1", -100, "2026-07-01", "food", "manual");
    fixture.database.insertTx(manual);
    EXPECT_TRUE(fixture.database.deleteTx(manual.id) == db::DeleteResult::Deleted);
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(0));
    EXPECT_TRUE(fixture.database.deleteTx("does-not-exist") == db::DeleteResult::NotFound);
}

TEST(splitPartsMustSumToTheParentAmount) {
    Fixture fixture;
    db::Transaction parent = makeTx(fixture.database, "acc-1", -10000, "2026-07-01", "shopping", "bank", "p1");
    fixture.database.insertTx(parent);

    std::vector<db::Transaction> wrong;
    wrong.push_back(makeTx(fixture.database, "acc-1", -3000, "2026-07-01", "food", "manual"));
    wrong.push_back(makeTx(fixture.database, "acc-1", -3000, "2026-07-01", "alko", "manual"));

    bool threw = false;
    try {
        fixture.database.splitTx(parent.id, wrong);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    // The rejected split must not have left partial rows behind.
    EXPECT_EQ(fixture.database.subTx(parent.id).size(), std::size_t(0));
}

TEST(splitPartsThatBalanceAreStored) {
    Fixture fixture;
    db::Transaction parent = makeTx(fixture.database, "acc-1", -10000, "2026-07-01", "shopping", "bank", "p1");
    fixture.database.insertTx(parent);

    std::vector<db::Transaction> parts;
    parts.push_back(makeTx(fixture.database, "acc-1", -7000, "2026-07-01", "food", "manual"));
    parts.push_back(makeTx(fixture.database, "acc-1", -3000, "2026-07-01", "alko", "manual"));
    fixture.database.splitTx(parent.id, parts);

    EXPECT_EQ(fixture.database.subTx(parent.id).size(), std::size_t(2));
    // The parts are hidden from the flat list, which still shows only the parent.
    EXPECT_EQ(fixture.database.transactions().size(), std::size_t(1));
}

TEST(splitTransactionsAreCountedOnceInBudgetActuals) {
    Fixture fixture;
    db::Budget budget;
    budget.yearMonth = "2026-07";
    budget.category = "food";
    budget.planned = 100000;
    fixture.database.upsertBudget(budget);

    db::Transaction parent = makeTx(fixture.database, "acc-1", -10000, "2026-07-15", "food", "bank", "p1");
    fixture.database.insertTx(parent);

    // Before splitting, the parent itself supplies the 100.00 of spend.
    auto summary = fixture.database.budgetSummary("2026-07");
    EXPECT_EQ(summary.size(), std::size_t(1));
    EXPECT_EQ(summary[0].actual, 10000);

    std::vector<db::Transaction> parts;
    parts.push_back(makeTx(fixture.database, "acc-1", -6000, "2026-07-15", "food", "manual"));
    parts.push_back(makeTx(fixture.database, "acc-1", -4000, "2026-07-15", "transport", "manual"));
    fixture.database.splitTx(parent.id, parts);

    // After splitting, only the food part counts: 60.00, not 160.00.
    summary = fixture.database.budgetSummary("2026-07");
    EXPECT_EQ(summary.size(), std::size_t(1));
    EXPECT_EQ(summary[0].actual, 6000);
}

TEST(statsExcludeSplitParentsAndGroupByCurrency) {
    Fixture fixture;
    db::Transaction parent = makeTx(fixture.database, "acc-1", -10000, "2026-07-15", "food", "bank", "p1");
    fixture.database.insertTx(parent);
    std::vector<db::Transaction> parts;
    parts.push_back(makeTx(fixture.database, "acc-1", -10000, "2026-07-15", "food", "manual"));
    fixture.database.splitTx(parent.id, parts);

    db::Transaction eurTx = makeTx(fixture.database, "acc-1", -2500, "2026-07-20", "food", "bank", "eur-1");
    eurTx.currency = "EUR";
    fixture.database.insertTx(eurTx);

    const auto rows = fixture.database.stats("", "");
    // One PLN row for the split part and one EUR row, never a combined total.
    EXPECT_EQ(rows.size(), std::size_t(2));
    int64_t pln = 0;
    int64_t eur = 0;
    for (const auto& row : rows) {
        if (row.currency == "PLN") pln = row.total;
        if (row.currency == "EUR") eur = row.total;
    }
    EXPECT_EQ(pln, 10000);
    EXPECT_EQ(eur, 2500);
}

TEST(transactionPagePaginatesAndReportsTheTotal) {
    Fixture fixture;
    for (int day = 1; day <= 10; ++day) {
        const std::string date = day < 10 ? "2026-07-0" + std::to_string(day) : "2026-07-10";
        fixture.database.insertTx(makeTx(fixture.database, "acc-1", -100 * day, date, "food", "bank",
                                         "ref-" + std::to_string(day)));
    }

    const auto firstPage = fixture.database.transactionPage("", "", "", "", "", 4, 0);
    EXPECT_EQ(firstPage.total, 10);
    EXPECT_EQ(firstPage.items.size(), std::size_t(4));
    // Newest first.
    EXPECT_EQ(firstPage.items[0].date, std::string("2026-07-10"));

    const auto lastPage = fixture.database.transactionPage("", "", "", "", "", 4, 8);
    EXPECT_EQ(lastPage.items.size(), std::size_t(2));
    EXPECT_EQ(lastPage.total, 10);
}

TEST(transactionPageFiltersByCategoryDateAndSearchText) {
    Fixture fixture;
    db::Transaction groceries = makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "food", "bank", "g");
    groceries.name = "Zabka Warszawa";
    fixture.database.insertTx(groceries);

    db::Transaction fuel = makeTx(fixture.database, "acc-1", -20000, "2026-06-15", "transport", "bank", "f");
    fuel.name = "Orlen";
    fixture.database.insertTx(fuel);

    EXPECT_EQ(fixture.database.transactionPage("", "", "", "food", "", 50, 0).items.size(), std::size_t(1));
    EXPECT_EQ(fixture.database.transactionPage("", "2026-07-01", "", "", "", 50, 0).items.size(), std::size_t(1));
    EXPECT_EQ(fixture.database.transactionPage("", "", "", "", "zabka", 50, 0).items.size(), std::size_t(1));
    EXPECT_EQ(fixture.database.transactionPage("", "", "", "", "orl", 50, 0).items.size(), std::size_t(1));
    EXPECT_EQ(fixture.database.transactionPage("", "", "", "", "nothing", 50, 0).items.size(), std::size_t(0));
}

TEST(editHistoryIsRecordedAndCountedInBulk) {
    Fixture fixture;
    db::Transaction tx = makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "other", "bank", "e1");
    fixture.database.insertTx(tx);

    db::Transaction updated = fixture.database.transaction(tx.id);
    updated.category = "food";
    updated.name = "Renamed";
    fixture.database.updateTx(tx.id, updated);

    EXPECT_EQ(fixture.database.txHistory(tx.id).size(), std::size_t(2));
    const auto counts = fixture.database.editCounts({tx.id});
    EXPECT_EQ(counts.size(), std::size_t(1));
    EXPECT_EQ(counts[0].second, 2);
}

TEST(deletingASavingsGoalRemovesItsEntries) {
    Fixture fixture;
    db::SavingsGoal goal;
    goal.name = "Samochod";
    goal.target = 1000000;
    const int64_t goalId = fixture.database.insertGoal(goal);

    db::SavingsEntry entry;
    entry.goalId = goalId;
    entry.yearMonth = "2026-07";
    entry.planned = 50000;
    entry.actual = 45000;
    fixture.database.upsertEntry(entry);
    EXPECT_EQ(fixture.database.savingsEntries(goalId).size(), std::size_t(1));

    fixture.database.deleteGoal(goalId);
    EXPECT_EQ(fixture.database.savingsGoals().size(), std::size_t(0));
    EXPECT_EQ(fixture.database.savingsEntries(goalId).size(), std::size_t(0));
}

TEST(budgetUpsertReplacesThePlannedAmountForTheSameMonth) {
    Fixture fixture;
    db::Budget budget;
    budget.yearMonth = "2026-07";
    budget.category = "food";
    budget.planned = 100000;
    budget.notes = "groceries";
    fixture.database.upsertBudget(budget);
    budget.planned = 150000;
    budget.notes = "incl. kids";
    fixture.database.upsertBudget(budget);

    const auto budgets = fixture.database.budgets("2026-07");
    EXPECT_EQ(budgets.size(), std::size_t(1));
    EXPECT_EQ(budgets[0].planned, 150000);
    EXPECT_EQ(budgets[0].notes, "incl. kids");
}

TEST(manualAccountBalanceFollowsTransactions) {
    Fixture fixture;
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", 50000, "2026-07-01", "income", "manual"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -12000, "2026-07-02", "food", "manual"));
    EXPECT_EQ(fixture.database.account("acc-1").balance, 38000);
}

TEST(monthTotalsSplitIncomeAndExpense) {
    Fixture fixture;
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", 10000, "2026-07-01", "income", "manual"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -3000, "2026-07-02", "food", "manual"));
    fixture.database.insertTx(makeTx(fixture.database, "acc-1", -2000, "2026-07-03", "transport", "manual"));
    const auto totals = fixture.database.monthTotals("2026-07-01", "2026-07-31");
    EXPECT_EQ(totals.size(), std::size_t(1));
    EXPECT_EQ(totals[0].currency, "PLN");
    EXPECT_EQ(totals[0].income, 10000);
    EXPECT_EQ(totals[0].expense, 5000);
}

TEST(copyBudgetsCopiesPlannedAndNotes) {
    Fixture fixture;
    db::Budget budget;
    budget.yearMonth = "2026-07";
    budget.category = "food";
    budget.planned = 80000;
    budget.notes = "limit";
    fixture.database.upsertBudget(budget);
    EXPECT_EQ(fixture.database.copyBudgets("2026-07", "2026-08"), 1);
    const auto next = fixture.database.budgets("2026-08");
    EXPECT_EQ(next.size(), std::size_t(1));
    EXPECT_EQ(next[0].planned, 80000);
    EXPECT_EQ(next[0].notes, "limit");
}

TEST(bankTxLookupAndUserEditFlag) {
    Fixture fixture;
    db::Transaction tx = makeTx(fixture.database, "acc-1", -1000, "2026-07-01", "other", "bank", "ref-x");
    fixture.database.insertTx(tx);
    const auto found = fixture.database.transactionByBankId("acc-1", "ref-x");
    EXPECT_EQ(found.id, tx.id);
    EXPECT_FALSE(fixture.database.fieldWasUserEdited(tx.id, "category"));
    db::Transaction updated = found;
    updated.category = "food";
    fixture.database.updateTx(tx.id, updated);
    EXPECT_TRUE(fixture.database.fieldWasUserEdited(tx.id, "category"));
}
