#include "database.h"
#include <sqlite3.h>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace db {

// RAII wrapper for sqlite3_stmt to prevent leaks.
struct Stmt {
    sqlite3_stmt* st = nullptr;
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { if (st) sqlite3_finalize(st); }
    void bindText(int i, const std::string& v) { sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT); }
    void bindInt64(int i, int64_t v) { sqlite3_bind_int64(st, i, v); }
    void bindInt(int i, int v) { sqlite3_bind_int(st, i, v); }
    int step() { return sqlite3_step(st); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

Database::Database(const std::string& path){
    if(sqlite3_open(path.c_str(),&db_)!=SQLITE_OK)
        throw std::runtime_error(std::string("sqlite open: ")+sqlite3_errmsg(db_));
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    init();
}
Database::~Database(){if(db_)sqlite3_close(db_);}

void Database::exec(const std::string& sql)const{
    char*err=nullptr;
    if(sqlite3_exec(db_,sql.c_str(),nullptr,nullptr,&err)!=SQLITE_OK){
        std::string msg=err?err:"unknown";sqlite3_free(err);
        throw std::runtime_error("sql error: "+msg+" in: "+sql.substr(0,200));
    }
}

std::string Database::now(){
    auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tm{};gmtime_r(&t,&tm);char b[32];
    std::strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",&tm);return b;
}

std::string Database::uuid()const{
    std::random_device rd;std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0,0xFFFFFFFF);
    std::ostringstream o;o<<std::hex<<std::setfill('0');
    uint32_t a=dist(gen),b=dist(gen),c=dist(gen),d=dist(gen);
    o<<std::setw(8)<<a<<"-"<<std::setw(4)<<(b>>16)<<"-4"<<std::setw(3)<<(b&0xFFF)
     <<"-"<<std::setw(4)<<((c&0x3FFF)|0x8000)<<"-"<<std::setw(8)<<(c>>16)<<std::setw(4)<<(d&0xFFFF);
    return o.str();
}

void Database::init(){
    exec(R"(CREATE TABLE IF NOT EXISTS accounts(
        id TEXT PRIMARY KEY,name TEXT NOT NULL,type TEXT NOT NULL DEFAULT 'bank',
        currency TEXT DEFAULT 'PLN',bank_name TEXT,iban TEXT,color TEXT,
        balance INTEGER DEFAULT 0,created_at TEXT,updated_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS transactions(
        id TEXT PRIMARY KEY,account_id TEXT NOT NULL,name TEXT,description TEXT,
        amount INTEGER NOT NULL,currency TEXT DEFAULT 'PLN',
        from_party TEXT,to_party TEXT,type TEXT NOT NULL DEFAULT 'expense',
        category TEXT DEFAULT 'other',tag TEXT DEFAULT 'opt',
        date TEXT NOT NULL,source TEXT DEFAULT 'bank',bank_tx_id TEXT,
        parent_id TEXT,created_at TEXT,updated_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS transaction_edits(
        id INTEGER PRIMARY KEY AUTOINCREMENT,tx_id TEXT NOT NULL,
        field TEXT NOT NULL,old_value TEXT,new_value TEXT,edited_at TEXT NOT NULL))");
    exec(R"(CREATE TABLE IF NOT EXISTS categories(
        name TEXT PRIMARY KEY,icon TEXT,color TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS budgets(
        id INTEGER PRIMARY KEY AUTOINCREMENT,year_month TEXT NOT NULL,
        category TEXT NOT NULL,planned INTEGER NOT NULL,UNIQUE(year_month,category)))");
    exec(R"(CREATE TABLE IF NOT EXISTS todos(
        id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,description TEXT,
        amount INTEGER,due_date TEXT,done INTEGER DEFAULT 0,created_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS savings_goals(
        id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,
        target INTEGER NOT NULL,deadline TEXT,created_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS savings_entries(
        id INTEGER PRIMARY KEY AUTOINCREMENT,goal_id INTEGER NOT NULL,
        year_month TEXT NOT NULL,planned INTEGER NOT NULL,actual INTEGER DEFAULT 0,
        UNIQUE(goal_id,year_month)))");
    exec(R"(CREATE TABLE IF NOT EXISTS sync_history(
        id INTEGER PRIMARY KEY AUTOINCREMENT,synced_at TEXT NOT NULL,
        bank_name TEXT,new_tx_count INTEGER DEFAULT 0,details TEXT))");
    // seed default categories
    struct CatDef { const char* name; const char* icon; const char* color; };
    const CatDef cats[] = {
        {"food", "mdi-food", "#FF6B6B"},
        {"transport", "mdi-car", "#4ECDC4"},
        {"entertainment", "mdi-movie", "#45B7D1"},
        {"utilities", "mdi-flash", "#FDCB6E"},
        {"health", "mdi-hospital", "#6C5CE7"},
        {"shopping", "mdi-cart", "#FF9FF3"},
        {"alko", "mdi-glass-wine", "#D63031"},
        {"wyjazdy", "mdi-airplane", "#00CEC9"},
        {"savings", "mdi-piggy-bank", "#00B894"},
        {"income", "mdi-cash-plus", "#27AE60"},
        {"transfer", "mdi-swap-horizontal", "#95A5A6"},
        {"other", "mdi-dots-horizontal", "#BDC3C7"},
        {nullptr, nullptr, nullptr}
    };
    for(int i=0; cats[i].name; ++i){
        Stmt s(db_, "INSERT OR IGNORE INTO categories(name,icon,color)VALUES(?,?,?)");
        s.bindText(1, cats[i].name);
        s.bindText(2, cats[i].icon);
        s.bindText(3, cats[i].color);
        s.step();
    }
}

// ===== ACCOUNTS =====
std::vector<Account> Database::accounts()const{
    std::vector<Account> out;
    sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT id,name,type,currency,bank_name,iban,color,balance,created_at,updated_at FROM accounts ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Account a;
        auto col=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        a.id=col(0);a.name=col(1);a.type=col(2);
        a.currency=sqlite3_column_text(st,3)?(const char*)sqlite3_column_text(st,3):"PLN";
        a.bankName=col(4);a.iban=col(5);a.color=col(6);
        a.balance=sqlite3_column_int64(st,7);
        a.createdAt=col(8);a.updatedAt=col(9);
        out.push_back(a);
    }
    sqlite3_finalize(st);return out;
}

void Database::upsertAccount(const Account& a){
    static const char* sql=
        "INSERT INTO accounts(id,name,type,currency,bank_name,iban,color,balance,created_at,updated_at)"
        "VALUES(?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,type=excluded.type,"
        "currency=excluded.currency,bank_name=excluded.bank_name,iban=excluded.iban,"
        "color=excluded.color,balance=excluded.balance,updated_at=excluded.updated_at";
    Stmt s(db_, sql);
    const std::string created=a.createdAt.empty()?now():a.createdAt;
    s.bindText(1,a.id);s.bindText(2,a.name);s.bindText(3,a.type);s.bindText(4,a.currency);
    s.bindText(5,a.bankName);s.bindText(6,a.iban);s.bindText(7,a.color);s.bindInt64(8,a.balance);
    s.bindText(9,created);s.bindText(10,now());
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("upsertAccount: ")+sqlite3_errmsg(db_));
}

void Database::deleteAccount(const std::string& id){
    Stmt s(db_, "DELETE FROM accounts WHERE id=?");
    s.bindText(1, id);
    s.step();
}

// ===== TRANSACTIONS =====
static Transaction readTx(sqlite3_stmt*st){
    Transaction t;
    auto col=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
    t.id=col(0);t.accountId=col(1);t.name=col(2);t.description=col(3);
    t.amount=sqlite3_column_int64(st,4);t.currency=col(5);t.fromParty=col(6);t.toParty=col(7);
    t.type=col(8);t.category=col(9);t.tag=col(10);t.date=col(11);t.source=col(12);
    t.bankTxId=col(13);t.parentId=col(14);t.createdAt=col(15);t.updatedAt=col(16);
    return t;
}

std::vector<Transaction> Database::transactions(const std::string& acct,const std::string& from,const std::string& to,int lim)const{
    std::string sql="SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE parent_id IS NULL OR parent_id=''";
    int nextBind=1;
    if(!acct.empty()){sql+=" AND account_id=?"; }
    if(!from.empty()){sql+=" AND date>=?"; }
    if(!to.empty()){sql+=" AND date<=?"; }
    sql+=" ORDER BY date DESC LIMIT ?";
    Stmt s(db_, sql);
    if(!acct.empty()) s.bindText(nextBind++, acct);
    if(!from.empty()) s.bindText(nextBind++, from);
    if(!to.empty()) s.bindText(nextBind++, to);
    s.bindInt(nextBind, lim);
    std::vector<Transaction> out;
    while(sqlite3_step(s.st)==SQLITE_ROW)out.push_back(readTx(s.st));
    return out;
}

Transaction Database::transaction(const std::string& id)const{
    Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE id=?");
    s.bindText(1, id);
    Transaction t;if(sqlite3_step(s.st)==SQLITE_ROW)t=readTx(s.st);
    return t;
}

void Database::insertTx(const Transaction& t){
    static const char* sql=
        "INSERT OR IGNORE INTO transactions(id,account_id,name,description,amount,currency,"
        "from_party,to_party,type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    Stmt s(db_, sql);
    const std::string created=t.createdAt.empty()?now():t.createdAt;
    const std::string updated=now();
    s.bindText(1,t.id);s.bindText(2,t.accountId);s.bindText(3,t.name);s.bindText(4,t.description);
    s.bindInt64(5,t.amount);s.bindText(6,t.currency);s.bindText(7,t.fromParty);s.bindText(8,t.toParty);
    s.bindText(9,t.type);s.bindText(10,t.category);s.bindText(11,t.tag);s.bindText(12,t.date);s.bindText(13,t.source);
    s.bindText(14,t.bankTxId);s.bindText(15,t.parentId);s.bindText(16,created);s.bindText(17,updated);
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("insertTx: ")+sqlite3_errmsg(db_));
}

void Database::updateTx(const std::string& id,const Transaction& u){
    Transaction old=transaction(id);
    auto check=[&](const std::string& f,const std::string& ov,const std::string& nv){
        if(ov!=nv){
            Stmt s(db_, "INSERT INTO transaction_edits(tx_id,field,old_value,new_value,edited_at)VALUES(?,?,?,?,?)");
            s.bindText(1,id);s.bindText(2,f);s.bindText(3,ov);s.bindText(4,nv);s.bindText(5,now());
            s.step();
        }
    };
    check("name",old.name,u.name);check("description",old.description,u.description);
    check("amount",std::to_string(old.amount),std::to_string(u.amount));
    check("category",old.category,u.category);check("tag",old.tag,u.tag);
    check("from_party",old.fromParty,u.fromParty);check("to_party",old.toParty,u.toParty);
    check("type",old.type,u.type);check("date",old.date,u.date);
    Stmt s(db_, "UPDATE transactions SET name=?,description=?,amount=?,category=?,tag=?,"
        "from_party=?,to_party=?,type=?,date=?,updated_at=? WHERE id=?");
    s.bindText(1,u.name);s.bindText(2,u.description);s.bindInt64(3,u.amount);
    s.bindText(4,u.category);s.bindText(5,u.tag);s.bindText(6,u.fromParty);s.bindText(7,u.toParty);
    s.bindText(8,u.type);s.bindText(9,u.date);s.bindText(10,now());s.bindText(11,id);
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("updateTx: ")+sqlite3_errmsg(db_));
}

void Database::deleteTx(const std::string& id){
    { Stmt s(db_, "DELETE FROM transactions WHERE id=? AND source='manual'"); s.bindText(1,id); s.step(); }
    { Stmt s(db_, "DELETE FROM transactions WHERE parent_id=?"); s.bindText(1,id); s.step(); }
    { Stmt s(db_, "DELETE FROM transaction_edits WHERE tx_id=?"); s.bindText(1,id); s.step(); }
}

bool Database::txExists(const std::string& bankTxId)const{
    Stmt s(db_, "SELECT 1 FROM transactions WHERE bank_tx_id=?");
    s.bindText(1, bankTxId);
    return sqlite3_step(s.st)==SQLITE_ROW;
}

std::vector<Transaction> Database::subTx(const std::string& pid)const{
    Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE parent_id=?");
    s.bindText(1, pid);
    std::vector<Transaction> out;
    while(sqlite3_step(s.st)==SQLITE_ROW)out.push_back(readTx(s.st));
    return out;
}

void Database::splitTx(const std::string& pid,const std::vector<Transaction>& parts){
    { Stmt s(db_, "DELETE FROM transactions WHERE parent_id=?"); s.bindText(1,pid); s.step(); }
    for(auto& p:parts){Transaction t=p;t.parentId=pid;if(t.id.empty())t.id=uuid();insertTx(t);}
}

std::vector<TxEdit> Database::txHistory(const std::string& tid)const{
    Stmt s(db_, "SELECT id,tx_id,field,old_value,new_value,edited_at FROM transaction_edits WHERE tx_id=? ORDER BY edited_at");
    s.bindText(1, tid);
    std::vector<TxEdit> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        TxEdit e;e.id=sqlite3_column_int64(s.st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        e.txId=c(1);e.field=c(2);e.oldVal=c(3);e.newVal=c(4);e.editedAt=c(5);
        out.push_back(e);
    }
    return out;
}

// ===== CATEGORIES =====
std::vector<Category> Database::categories()const{
    std::vector<Category> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT name,icon,color FROM categories ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Category c;auto col=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        c.name=col(0);c.icon=col(1);c.color=col(2);out.push_back(c);
    }
    sqlite3_finalize(st);return out;
}
void Database::upsertCategory(const Category& c){
    Stmt s(db_, "INSERT INTO categories(name,icon,color)VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET icon=excluded.icon,color=excluded.color");
    s.bindText(1,c.name);s.bindText(2,c.icon);s.bindText(3,c.color);
    s.step();
}

// ===== BUDGETS =====
std::vector<Budget> Database::budgets(const std::string& ym)const{
    Stmt s(db_, "SELECT id,year_month,category,planned FROM budgets WHERE year_month=?");
    s.bindText(1, ym);
    std::vector<Budget> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        Budget b;b.id=sqlite3_column_int64(s.st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        b.yearMonth=c(1);b.category=c(2);b.planned=sqlite3_column_int64(s.st,3);out.push_back(b);
    }
    return out;
}
void Database::upsertBudget(const Budget& b){
    Stmt s(db_, "INSERT INTO budgets(year_month,category,planned)VALUES(?,?,?) ON CONFLICT(year_month,category) DO UPDATE SET planned=excluded.planned");
    s.bindText(1,b.yearMonth);s.bindText(2,b.category);s.bindInt64(3,b.planned);
    s.step();
}
std::vector<BudgetLine> Database::budgetSummary(const std::string& ym)const{
    Stmt s(db_, "SELECT b.category,b.planned,COALESCE(SUM(ABS(t.amount)),0) FROM budgets b "
        "LEFT JOIN transactions t ON t.category=b.category AND t.type='expense' AND substr(t.date,1,7)=b.year_month "
        "WHERE b.year_month=? GROUP BY b.category");
    s.bindText(1, ym);
    std::vector<BudgetLine> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        BudgetLine l;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        l.category=c(0);l.planned=sqlite3_column_int64(s.st,1);l.actual=sqlite3_column_int64(s.st,2);out.push_back(l);
    }
    return out;
}

// ===== TODOS =====
std::vector<Todo> Database::todos()const{
    std::vector<Todo> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT id,name,description,amount,due_date,done,created_at FROM todos ORDER BY done,due_date",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Todo t;t.id=sqlite3_column_int64(st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        t.name=c(1);t.description=c(2);t.amount=sqlite3_column_int64(st,3);
        t.dueDate=c(4);t.done=sqlite3_column_int(st,5)!=0;t.createdAt=c(6);out.push_back(t);
    }
    sqlite3_finalize(st);return out;
}
int64_t Database::insertTodo(const Todo& t){
    Stmt s(db_, "INSERT INTO todos(name,description,amount,due_date,done,created_at)VALUES(?,?,?,?,0,?)");
    s.bindText(1,t.name);s.bindText(2,t.description);s.bindInt64(3,t.amount);s.bindText(4,t.dueDate);s.bindText(5,now());
    s.step();
    return sqlite3_last_insert_rowid(db_);
}
void Database::updateTodo(int64_t id,const Todo& t){
    Stmt s(db_, "UPDATE todos SET name=?,description=?,amount=?,due_date=?,done=? WHERE id=?");
    s.bindText(1,t.name);s.bindText(2,t.description);s.bindInt64(3,t.amount);s.bindText(4,t.dueDate);
    s.bindInt(5,t.done?1:0);s.bindInt64(6,id);
    s.step();
}
void Database::deleteTodo(int64_t id){
    Stmt s(db_, "DELETE FROM todos WHERE id=?");
    s.bindInt64(1, id);
    s.step();
}

// ===== SAVINGS =====
std::vector<SavingsGoal> Database::savingsGoals()const{
    std::vector<SavingsGoal> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT id,name,target,deadline,created_at FROM savings_goals ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        SavingsGoal g;g.id=sqlite3_column_int64(st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        g.name=c(1);g.target=sqlite3_column_int64(st,2);g.deadline=c(3);g.createdAt=c(4);out.push_back(g);
    }
    sqlite3_finalize(st);return out;
}
int64_t Database::insertGoal(const SavingsGoal& g){
    Stmt s(db_, "INSERT INTO savings_goals(name,target,deadline,created_at)VALUES(?,?,?,?)");
    s.bindText(1,g.name);s.bindInt64(2,g.target);s.bindText(3,g.deadline);s.bindText(4,now());
    s.step();
    return sqlite3_last_insert_rowid(db_);
}
void Database::deleteGoal(int64_t id){
    { Stmt s(db_, "DELETE FROM savings_entries WHERE goal_id=?"); s.bindInt64(1,id); s.step(); }
    { Stmt s(db_, "DELETE FROM savings_goals WHERE id=?"); s.bindInt64(1,id); s.step(); }
}
void Database::upsertEntry(const SavingsEntry& e){
    Stmt s(db_, "INSERT INTO savings_entries(goal_id,year_month,planned,actual)VALUES(?,?,?,?)"
        " ON CONFLICT(goal_id,year_month) DO UPDATE SET planned=excluded.planned,actual=excluded.actual");
    s.bindInt64(1,e.goalId);s.bindText(2,e.yearMonth);s.bindInt64(3,e.planned);s.bindInt64(4,e.actual);
    s.step();
}
std::vector<SavingsEntry> Database::savingsEntries(int64_t gid)const{
    Stmt s(db_, "SELECT id,goal_id,year_month,planned,actual FROM savings_entries WHERE goal_id=? ORDER BY year_month");
    s.bindInt64(1, gid);
    std::vector<SavingsEntry> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        SavingsEntry e;e.id=sqlite3_column_int64(s.st,0);e.goalId=sqlite3_column_int64(s.st,1);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        e.yearMonth=c(2);e.planned=sqlite3_column_int64(s.st,3);e.actual=sqlite3_column_int64(s.st,4);out.push_back(e);
    }
    return out;
}

// ===== SYNC =====
void Database::recordSync(const SyncRec& r){
    Stmt s(db_, "INSERT INTO sync_history(synced_at,bank_name,new_tx_count,details)VALUES(?,?,?,?)");
    s.bindText(1,r.syncedAt.empty()?now():r.syncedAt);s.bindText(2,r.bankName);s.bindInt(3,r.newTx);s.bindText(4,r.details);
    s.step();
}
std::vector<SyncRec> Database::syncHistory(int lim)const{
    Stmt s(db_, "SELECT id,synced_at,bank_name,new_tx_count,details FROM sync_history ORDER BY id DESC LIMIT ?");
    s.bindInt(1, lim);
    std::vector<SyncRec> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        SyncRec r;r.id=sqlite3_column_int64(s.st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        r.syncedAt=c(1);r.bankName=c(2);r.newTx=sqlite3_column_int(s.st,3);r.details=c(4);out.push_back(r);
    }
    return out;
}

// ===== STATS =====
std::vector<StatsRow> Database::stats(const std::string& from,const std::string& to)const{
    std::string sql="SELECT category,substr(date,1,7) as ym,SUM(ABS(amount)) FROM transactions WHERE type='expense'";
    int nextBind=1;
    if(!from.empty()){sql+=" AND date>=?"; }
    if(!to.empty()){sql+=" AND date<=?"; }
    sql+=" GROUP BY category,ym ORDER BY ym,category";
    Stmt s(db_, sql);
    if(!from.empty()) s.bindText(nextBind++, from);
    if(!to.empty()) s.bindText(nextBind++, to);
    std::vector<StatsRow> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        StatsRow r;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        r.category=c(0);r.yearMonth=c(1);r.total=sqlite3_column_int64(s.st,2);out.push_back(r);
    }
    return out;
}

} // namespace db
