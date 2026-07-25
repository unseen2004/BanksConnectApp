#ifndef BANKSCONNECTAPP_DATABASE_H
#define BANKSCONNECTAPP_DATABASE_H
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
struct sqlite3;
namespace db {
struct Account {
    std::string id,name,type,currency,bankName,iban,color,source,createdAt,updatedAt;
    int64_t balance=0;
};
struct Transaction { std::string id,accountId,name,description,fromParty,toParty,type,category,tag,date,source,bankTxId,parentId,currency,createdAt,updatedAt; int64_t amount=0; };
struct TxEdit { int64_t id=0; std::string txId,field,oldVal,newVal,editedAt; };
struct Category { std::string name,icon,color; };
struct Budget { int64_t id=0; std::string yearMonth,category,notes; int64_t planned=0; };
struct BudgetLine { std::string category,notes; int64_t planned=0,actual=0; };
struct Todo { int64_t id=0; std::string name,description,dueDate,createdAt; int64_t amount=0; bool done=false; };
struct SavingsGoal { int64_t id=0; std::string name,deadline,createdAt; int64_t target=0; };
struct SavingsEntry { int64_t id=0,goalId=0,planned=0,actual=0; std::string yearMonth; };
struct SyncRec { int64_t id=0; std::string syncedAt,bankName,details; int newTx=0; };
struct StatsRow { std::string category,yearMonth,currency; int64_t total=0; };
struct MonthTotals { std::string currency; int64_t income=0, expense=0; };

/// Outcome of an attempted transaction delete. Bank-sourced rows are read-only:
/// removing one locally would only make it reappear on the next sync, so callers
/// need to tell that case apart from a successful delete.
enum class DeleteResult { Deleted, NotFound, NotDeletable };

/// A page of transactions plus the total number of matching rows, so a client can
/// tell whether more pages exist.
struct TransactionPage {
    std::vector<Transaction> items;
    int64_t total = 0;
};

// Thread-safe: the background sync thread and the HTTP request handlers share
// one instance, and a single sqlite3 connection must not be used concurrently.
// Every public method serializes on mutex_. It is recursive because some methods
// are implemented in terms of others (updateTx -> transaction, splitTx -> insertTx).
class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    // accounts
    std::vector<Account> accounts() const;
    Account account(const std::string& id) const;
    void upsertAccount(const Account& a);
    void deleteAccount(const std::string& id);
    /// Sum of top-level transaction amounts → accounts.balance (manual accounts).
    void recalcAccountBalance(const std::string& accountId);
    // transactions
    std::vector<Transaction> transactions(const std::string& acct="",const std::string& from="",const std::string& to="",int lim=500) const;
    /// Paginated variant. `category` and `search` are optional filters; `search`
    /// matches the name or description case-insensitively.
    TransactionPage transactionPage(const std::string& acct,const std::string& from,const std::string& to,
                                    const std::string& category,const std::string& search,
                                    int limit,int offset) const;
    Transaction transaction(const std::string& id) const;
    /// Lookup by Enable Banking reference for sync upserts.
    Transaction transactionByBankId(const std::string& accountId, const std::string& bankTxId) const;
    void insertTx(const Transaction& t);
    void updateTx(const std::string& id,const Transaction& t);
    DeleteResult deleteTx(const std::string& id);
    bool txExists(const std::string& accountId,const std::string& bankTxId) const;
    /// True when the user has edited this field (so sync should not overwrite it).
    bool fieldWasUserEdited(const std::string& txId, const std::string& field) const;
    /// Bulk lookup used by the sync loop to avoid one SELECT per incoming row.
    std::vector<std::string> existingBankTxIds(const std::string& accountId) const;
    std::vector<Transaction> subTx(const std::string& pid) const;
    /// Splits a transaction into parts. Throws std::invalid_argument when the parts
    /// do not sum to the parent amount, since a mismatch would silently change the
    /// account total.
    void splitTx(const std::string& pid,const std::vector<Transaction>& parts);
    std::vector<TxEdit> txHistory(const std::string& tid) const;
    /// Splits for many parents in one query, keyed by parent id.
    std::vector<Transaction> subTxForParents(const std::vector<std::string>& parentIds) const;
    /// Edit counts for many transactions in one query.
    std::vector<std::pair<std::string,int64_t>> editCounts(const std::vector<std::string>& txIds) const;
    // categories
    std::vector<Category> categories() const;
    void upsertCategory(const Category& c);
    // budgets
    std::vector<Budget> budgets(const std::string& ym) const;
    void upsertBudget(const Budget& b);
    std::vector<BudgetLine> budgetSummary(const std::string& ym) const;
    /// Copy planned (+ notes) from one month to another. Returns rows copied.
    int copyBudgets(const std::string& fromYm, const std::string& toYm);
    // todos
    std::vector<Todo> todos() const;
    int64_t insertTodo(const Todo& t);
    void updateTodo(int64_t id,const Todo& t);
    void deleteTodo(int64_t id);
    // savings
    std::vector<SavingsGoal> savingsGoals() const;
    int64_t insertGoal(const SavingsGoal& g);
    void deleteGoal(int64_t id);
    void upsertEntry(const SavingsEntry& e);
    std::vector<SavingsEntry> savingsEntries(int64_t gid) const;
    // sync
    void recordSync(const SyncRec& r);
    std::vector<SyncRec> syncHistory(int lim=50) const;
    // stats
    std::vector<StatsRow> stats(const std::string& from,const std::string& to) const;
    std::vector<MonthTotals> monthTotals(const std::string& from,const std::string& to) const;
    // util
    std::string uuid() const;
    static std::string now();
    /// Current on-disk schema version, for diagnostics and tests.
    int schemaVersion() const;
private:
    sqlite3* db_=nullptr;
    mutable std::recursive_mutex mutex_;
    void init();
    void exec(const std::string& sql) const;
    int readSchemaVersion() const;
    void writeSchemaVersion(int version) const;
    bool tableExists(const std::string& name) const;
    bool columnExists(const std::string& table, const std::string& column) const;
    void createSchema() const;
    void migrateToForeignKeys() const;
    void migrateToV3() const;
    void createIndexes() const;
    void seedCategories() const;
    void recalcAccountBalanceLocked(const std::string& accountId);
};
} // namespace db
#endif
