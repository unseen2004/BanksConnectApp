#ifndef BANKSCONNECTAPP_DATABASE_H
#define BANKSCONNECTAPP_DATABASE_H
#include <cstdint>
#include <string>
#include <vector>
struct sqlite3;
namespace db {
struct Account { std::string id,name,type,currency,bankName,iban,createdAt,updatedAt; int64_t balance=0; };
struct Transaction { std::string id,accountId,name,description,fromParty,toParty,type,category,tag,date,source,bankTxId,parentId,currency,createdAt,updatedAt; int64_t amount=0; };
struct TxEdit { int64_t id=0; std::string txId,field,oldVal,newVal,editedAt; };
struct Category { std::string name,icon,color; };
struct Budget { int64_t id=0; std::string yearMonth,category; int64_t planned=0; };
struct BudgetLine { std::string category; int64_t planned=0,actual=0; };
struct Todo { int64_t id=0; std::string name,description,dueDate,createdAt; int64_t amount=0; bool done=false; };
struct SavingsGoal { int64_t id=0; std::string name,deadline,createdAt; int64_t target=0; };
struct SavingsEntry { int64_t id=0,goalId=0,planned=0,actual=0; std::string yearMonth; };
struct SyncRec { int64_t id=0; std::string syncedAt,bankName,details; int newTx=0; };
struct StatsRow { std::string category,yearMonth; int64_t total=0; };

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    // accounts
    std::vector<Account> accounts() const;
    void upsertAccount(const Account& a);
    void deleteAccount(const std::string& id);
    // transactions
    std::vector<Transaction> transactions(const std::string& acct="",const std::string& from="",const std::string& to="",int lim=500) const;
    Transaction transaction(const std::string& id) const;
    void insertTx(const Transaction& t);
    void updateTx(const std::string& id,const Transaction& t);
    void deleteTx(const std::string& id);
    bool txExists(const std::string& bankTxId) const;
    std::vector<Transaction> subTx(const std::string& pid) const;
    void splitTx(const std::string& pid,const std::vector<Transaction>& parts);
    std::vector<TxEdit> txHistory(const std::string& tid) const;
    // categories
    std::vector<Category> categories() const;
    void upsertCategory(const Category& c);
    // budgets
    std::vector<Budget> budgets(const std::string& ym) const;
    void upsertBudget(const Budget& b);
    std::vector<BudgetLine> budgetSummary(const std::string& ym) const;
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
    // util
    std::string uuid() const;
    static std::string now();
private:
    sqlite3* db_=nullptr;
    void init();
    void exec(const std::string& sql) const;
};
} // namespace db
#endif
