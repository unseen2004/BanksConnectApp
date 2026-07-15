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

static std::string q(const std::string& v){std::string o="'";for(char c:v){if(c=='\'')o+="''";else o+=c;}o+="'";return o;}

Database::Database(const std::string& path){
    if(sqlite3_open(path.c_str(),&db_)!=SQLITE_OK)
        throw std::runtime_error(std::string("sqlite open: ")+sqlite3_errmsg(db_));
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
        currency TEXT DEFAULT 'PLN',bank_name TEXT,iban TEXT,
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
    const char* cats[]={"food","transport","entertainment","utilities","health",
        "shopping","alko","wyjazdy","savings","income","transfer","other",nullptr};
    for(int i=0;cats[i];++i)
        exec("INSERT OR IGNORE INTO categories(name)VALUES("+q(cats[i])+")");
}

// ===== ACCOUNTS =====
std::vector<Account> Database::accounts()const{
    std::vector<Account> out;
    sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT id,name,type,currency,bank_name,iban,balance,created_at,updated_at FROM accounts ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Account a;
        a.id=(const char*)sqlite3_column_text(st,0);
        a.name=(const char*)sqlite3_column_text(st,1);
        a.type=(const char*)sqlite3_column_text(st,2);
        a.currency=sqlite3_column_text(st,3)?(const char*)sqlite3_column_text(st,3):"PLN";
        a.bankName=sqlite3_column_text(st,4)?(const char*)sqlite3_column_text(st,4):"";
        a.iban=sqlite3_column_text(st,5)?(const char*)sqlite3_column_text(st,5):"";
        a.balance=sqlite3_column_int64(st,6);
        a.createdAt=sqlite3_column_text(st,7)?(const char*)sqlite3_column_text(st,7):"";
        a.updatedAt=sqlite3_column_text(st,8)?(const char*)sqlite3_column_text(st,8):"";
        out.push_back(a);
    }
    sqlite3_finalize(st);return out;
}

void Database::upsertAccount(const Account& a){
    exec("INSERT INTO accounts(id,name,type,currency,bank_name,iban,balance,created_at,updated_at)"
         "VALUES("+q(a.id)+","+q(a.name)+","+q(a.type)+","+q(a.currency)+","
         +q(a.bankName)+","+q(a.iban)+","+std::to_string(a.balance)+","
         +q(a.createdAt.empty()?now():a.createdAt)+","+q(now())+")"
         " ON CONFLICT(id) DO UPDATE SET name=excluded.name,type=excluded.type,"
         "currency=excluded.currency,bank_name=excluded.bank_name,iban=excluded.iban,"
         "balance=excluded.balance,updated_at=excluded.updated_at");
}

void Database::deleteAccount(const std::string& id){exec("DELETE FROM accounts WHERE id="+q(id));}

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
    if(!acct.empty())sql+=" AND account_id="+q(acct);
    if(!from.empty())sql+=" AND date>="+q(from);
    if(!to.empty())sql+=" AND date<="+q(to);
    sql+=" ORDER BY date DESC LIMIT "+std::to_string(lim);
    std::vector<Transaction> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,sql.c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW)out.push_back(readTx(st));
    sqlite3_finalize(st);return out;
}

Transaction Database::transaction(const std::string& id)const{
    sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE id="+q(id)).c_str(),-1,&st,nullptr);
    Transaction t;if(sqlite3_step(st)==SQLITE_ROW)t=readTx(st);
    sqlite3_finalize(st);return t;
}

void Database::insertTx(const Transaction& t){
    exec("INSERT OR IGNORE INTO transactions(id,account_id,name,description,amount,currency,"
        "from_party,to_party,type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at)"
        "VALUES("+q(t.id)+","+q(t.accountId)+","+q(t.name)+","+q(t.description)+","
        +std::to_string(t.amount)+","+q(t.currency)+","+q(t.fromParty)+","+q(t.toParty)+","
        +q(t.type)+","+q(t.category)+","+q(t.tag)+","+q(t.date)+","+q(t.source)+","
        +q(t.bankTxId)+","+q(t.parentId)+","+q(t.createdAt.empty()?now():t.createdAt)+","+q(now())+")");
}

void Database::updateTx(const std::string& id,const Transaction& u){
    Transaction old=transaction(id);
    auto check=[&](const std::string& f,const std::string& ov,const std::string& nv){
        if(ov!=nv)exec("INSERT INTO transaction_edits(tx_id,field,old_value,new_value,edited_at)"
            "VALUES("+q(id)+","+q(f)+","+q(ov)+","+q(nv)+","+q(now())+")");
    };
    check("name",old.name,u.name);check("description",old.description,u.description);
    check("amount",std::to_string(old.amount),std::to_string(u.amount));
    check("category",old.category,u.category);check("tag",old.tag,u.tag);
    check("from_party",old.fromParty,u.fromParty);check("to_party",old.toParty,u.toParty);
    check("type",old.type,u.type);check("date",old.date,u.date);
    exec("UPDATE transactions SET name="+q(u.name)+",description="+q(u.description)
        +",amount="+std::to_string(u.amount)+",category="+q(u.category)+",tag="+q(u.tag)
        +",from_party="+q(u.fromParty)+",to_party="+q(u.toParty)+",type="+q(u.type)
        +",date="+q(u.date)+",updated_at="+q(now())+" WHERE id="+q(id));
}

void Database::deleteTx(const std::string& id){
    exec("DELETE FROM transactions WHERE id="+q(id)+" AND source='manual'");
    exec("DELETE FROM transactions WHERE parent_id="+q(id));
    exec("DELETE FROM transaction_edits WHERE tx_id="+q(id));
}

bool Database::txExists(const std::string& bankTxId)const{
    sqlite3_stmt*st;bool found=false;
    sqlite3_prepare_v2(db_,("SELECT 1 FROM transactions WHERE bank_tx_id="+q(bankTxId)).c_str(),-1,&st,nullptr);
    if(sqlite3_step(st)==SQLITE_ROW)found=true;
    sqlite3_finalize(st);return found;
}

std::vector<Transaction> Database::subTx(const std::string& pid)const{
    std::vector<Transaction> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE parent_id="+q(pid)).c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW)out.push_back(readTx(st));
    sqlite3_finalize(st);return out;
}

void Database::splitTx(const std::string& pid,const std::vector<Transaction>& parts){
    exec("DELETE FROM transactions WHERE parent_id="+q(pid));
    for(auto& p:parts){Transaction t=p;t.parentId=pid;if(t.id.empty())t.id=uuid();insertTx(t);}
}

std::vector<TxEdit> Database::txHistory(const std::string& tid)const{
    std::vector<TxEdit> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,tx_id,field,old_value,new_value,edited_at FROM transaction_edits WHERE tx_id="+q(tid)+" ORDER BY edited_at").c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        TxEdit e;e.id=sqlite3_column_int64(st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        e.txId=c(1);e.field=c(2);e.oldVal=c(3);e.newVal=c(4);e.editedAt=c(5);
        out.push_back(e);
    }
    sqlite3_finalize(st);return out;
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
    exec("INSERT INTO categories(name,icon,color)VALUES("+q(c.name)+","+q(c.icon)+","+q(c.color)
        +") ON CONFLICT(name) DO UPDATE SET icon=excluded.icon,color=excluded.color");
}

// ===== BUDGETS =====
std::vector<Budget> Database::budgets(const std::string& ym)const{
    std::vector<Budget> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,year_month,category,planned FROM budgets WHERE year_month="+q(ym)).c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Budget b;b.id=sqlite3_column_int64(st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        b.yearMonth=c(1);b.category=c(2);b.planned=sqlite3_column_int64(st,3);out.push_back(b);
    }
    sqlite3_finalize(st);return out;
}
void Database::upsertBudget(const Budget& b){
    exec("INSERT INTO budgets(year_month,category,planned)VALUES("+q(b.yearMonth)+","+q(b.category)+","
        +std::to_string(b.planned)+") ON CONFLICT(year_month,category) DO UPDATE SET planned=excluded.planned");
}
std::vector<BudgetLine> Database::budgetSummary(const std::string& ym)const{
    std::vector<BudgetLine> out;sqlite3_stmt*st;
    std::string sql="SELECT b.category,b.planned,COALESCE(SUM(ABS(t.amount)),0) FROM budgets b "
        "LEFT JOIN transactions t ON t.category=b.category AND t.type='expense' AND substr(t.date,1,7)=b.year_month "
        "WHERE b.year_month="+q(ym)+" GROUP BY b.category";
    sqlite3_prepare_v2(db_,sql.c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        BudgetLine l;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        l.category=c(0);l.planned=sqlite3_column_int64(st,1);l.actual=sqlite3_column_int64(st,2);out.push_back(l);
    }
    sqlite3_finalize(st);return out;
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
    exec("INSERT INTO todos(name,description,amount,due_date,done,created_at)VALUES("
        +q(t.name)+","+q(t.description)+","+std::to_string(t.amount)+","+q(t.dueDate)+",0,"+q(now())+")");
    return sqlite3_last_insert_rowid(db_);
}
void Database::updateTodo(int64_t id,const Todo& t){
    exec("UPDATE todos SET name="+q(t.name)+",description="+q(t.description)+",amount="+std::to_string(t.amount)
        +",due_date="+q(t.dueDate)+",done="+std::to_string(t.done?1:0)+" WHERE id="+std::to_string(id));
}
void Database::deleteTodo(int64_t id){exec("DELETE FROM todos WHERE id="+std::to_string(id));}

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
    exec("INSERT INTO savings_goals(name,target,deadline,created_at)VALUES("
        +q(g.name)+","+std::to_string(g.target)+","+q(g.deadline)+","+q(now())+")");
    return sqlite3_last_insert_rowid(db_);
}
void Database::deleteGoal(int64_t id){
    exec("DELETE FROM savings_entries WHERE goal_id="+std::to_string(id));
    exec("DELETE FROM savings_goals WHERE id="+std::to_string(id));
}
void Database::upsertEntry(const SavingsEntry& e){
    exec("INSERT INTO savings_entries(goal_id,year_month,planned,actual)VALUES("
        +std::to_string(e.goalId)+","+q(e.yearMonth)+","+std::to_string(e.planned)+","+std::to_string(e.actual)
        +") ON CONFLICT(goal_id,year_month) DO UPDATE SET planned=excluded.planned,actual=excluded.actual");
}
std::vector<SavingsEntry> Database::savingsEntries(int64_t gid)const{
    std::vector<SavingsEntry> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,goal_id,year_month,planned,actual FROM savings_entries WHERE goal_id="
        +std::to_string(gid)+" ORDER BY year_month").c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        SavingsEntry e;e.id=sqlite3_column_int64(st,0);e.goalId=sqlite3_column_int64(st,1);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        e.yearMonth=c(2);e.planned=sqlite3_column_int64(st,3);e.actual=sqlite3_column_int64(st,4);out.push_back(e);
    }
    sqlite3_finalize(st);return out;
}

// ===== SYNC =====
void Database::recordSync(const SyncRec& r){
    exec("INSERT INTO sync_history(synced_at,bank_name,new_tx_count,details)VALUES("
        +q(r.syncedAt.empty()?now():r.syncedAt)+","+q(r.bankName)+","+std::to_string(r.newTx)+","+q(r.details)+")");
}
std::vector<SyncRec> Database::syncHistory(int lim)const{
    std::vector<SyncRec> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,("SELECT id,synced_at,bank_name,new_tx_count,details FROM sync_history ORDER BY id DESC LIMIT "+std::to_string(lim)).c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        SyncRec r;r.id=sqlite3_column_int64(st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        r.syncedAt=c(1);r.bankName=c(2);r.newTx=sqlite3_column_int(st,3);r.details=c(4);out.push_back(r);
    }
    sqlite3_finalize(st);return out;
}

// ===== STATS =====
std::vector<StatsRow> Database::stats(const std::string& from,const std::string& to)const{
    std::string sql="SELECT category,substr(date,1,7) as ym,SUM(ABS(amount)) FROM transactions WHERE type='expense'";
    if(!from.empty())sql+=" AND date>="+q(from);
    if(!to.empty())sql+=" AND date<="+q(to);
    sql+=" GROUP BY category,ym ORDER BY ym,category";
    std::vector<StatsRow> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,sql.c_str(),-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        StatsRow r;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        r.category=c(0);r.yearMonth=c(1);r.total=sqlite3_column_int64(st,2);out.push_back(r);
    }
    sqlite3_finalize(st);return out;
}

} // namespace db
