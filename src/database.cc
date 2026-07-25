#include "database.h"
#include <sqlite3.h>
#include <cstdlib>
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
    // Foreign key columns must hold NULL rather than '' when unset: an empty
    // string is a value, and it matches no parent row.
    void bindTextOrNull(int i, const std::string& v) {
        if (v.empty()) sqlite3_bind_null(st, i);
        else sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT);
    }
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

namespace {
// Bump when the schema changes and add a migration step in init().
//   1 - original schema, no declared foreign keys, no indexes
//   2 - foreign keys with ON DELETE CASCADE, supporting indexes, and a partial
//       unique index on (account_id, bank_tx_id) to enforce sync de-duplication
//   3 - accounts.source (bank|manual), budgets.notes
constexpr int kTargetSchemaVersion = 3;
}  // namespace

int Database::readSchemaVersion() const {
    if (!tableExists("schema_meta")) {
        // A database that already has data but no version marker predates
        // versioning; anything else is brand new.
        return tableExists("transactions") ? 1 : 0;
    }
    Stmt s(db_, "SELECT value FROM schema_meta WHERE key='version'");
    if (sqlite3_step(s.st) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(s.st, 0);
        if (text != nullptr) {
            return std::atoi(reinterpret_cast<const char*>(text));
        }
    }
    return tableExists("transactions") ? 1 : 0;
}

void Database::writeSchemaVersion(int version) const {
    Stmt s(db_, "INSERT INTO schema_meta(key,value)VALUES('version',?)"
                " ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.bindText(1, std::to_string(version));
    s.step();
}

int Database::schemaVersion() const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    return readSchemaVersion();
}

bool Database::tableExists(const std::string& name) const {
    Stmt s(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    s.bindText(1, name);
    return sqlite3_step(s.st) == SQLITE_ROW;
}

bool Database::columnExists(const std::string& table, const std::string& column) const {
    Stmt s(db_, "SELECT 1 FROM pragma_table_info(?) WHERE name=?");
    s.bindText(1, table);
    s.bindText(2, column);
    return sqlite3_step(s.st) == SQLITE_ROW;
}

void Database::createSchema() const {
    exec(R"(CREATE TABLE IF NOT EXISTS accounts(
        id TEXT PRIMARY KEY,name TEXT NOT NULL,type TEXT NOT NULL DEFAULT 'bank',
        currency TEXT DEFAULT 'PLN',bank_name TEXT,iban TEXT,color TEXT,
        balance INTEGER DEFAULT 0,source TEXT DEFAULT 'manual',
        created_at TEXT,updated_at TEXT))");
    // Deleting an account or a parent transaction must take its children with it,
    // otherwise the rows stay behind and keep contributing to totals.
    exec(R"(CREATE TABLE IF NOT EXISTS transactions(
        id TEXT PRIMARY KEY,account_id TEXT NOT NULL,name TEXT,description TEXT,
        amount INTEGER NOT NULL,currency TEXT DEFAULT 'PLN',
        from_party TEXT,to_party TEXT,type TEXT NOT NULL DEFAULT 'expense',
        category TEXT DEFAULT 'other',tag TEXT DEFAULT 'opt',
        date TEXT NOT NULL,source TEXT DEFAULT 'bank',bank_tx_id TEXT,
        parent_id TEXT,created_at TEXT,updated_at TEXT,
        FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE,
        FOREIGN KEY(parent_id) REFERENCES transactions(id) ON DELETE CASCADE))");
    exec(R"(CREATE TABLE IF NOT EXISTS transaction_edits(
        id INTEGER PRIMARY KEY AUTOINCREMENT,tx_id TEXT NOT NULL,
        field TEXT NOT NULL,old_value TEXT,new_value TEXT,edited_at TEXT NOT NULL,
        FOREIGN KEY(tx_id) REFERENCES transactions(id) ON DELETE CASCADE))");
    exec(R"(CREATE TABLE IF NOT EXISTS categories(
        name TEXT PRIMARY KEY,icon TEXT,color TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS budgets(
        id INTEGER PRIMARY KEY AUTOINCREMENT,year_month TEXT NOT NULL,
        category TEXT NOT NULL,planned INTEGER NOT NULL,notes TEXT DEFAULT '',
        UNIQUE(year_month,category)))");
    exec(R"(CREATE TABLE IF NOT EXISTS todos(
        id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,description TEXT,
        amount INTEGER,due_date TEXT,done INTEGER DEFAULT 0,created_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS savings_goals(
        id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL,
        target INTEGER NOT NULL,deadline TEXT,created_at TEXT))");
    exec(R"(CREATE TABLE IF NOT EXISTS savings_entries(
        id INTEGER PRIMARY KEY AUTOINCREMENT,goal_id INTEGER NOT NULL,
        year_month TEXT NOT NULL,planned INTEGER NOT NULL,actual INTEGER DEFAULT 0,
        UNIQUE(goal_id,year_month),
        FOREIGN KEY(goal_id) REFERENCES savings_goals(id) ON DELETE CASCADE))");
    exec(R"(CREATE TABLE IF NOT EXISTS sync_history(
        id INTEGER PRIMARY KEY AUTOINCREMENT,synced_at TEXT NOT NULL,
        bank_name TEXT,new_tx_count INTEGER DEFAULT 0,details TEXT))");
}

void Database::migrateToForeignKeys() const {
    // SQLite cannot add a foreign key to an existing table, so the three affected
    // tables are rebuilt. foreign_keys is disabled for the duration because the
    // intermediate states would otherwise violate the very constraints being added.
    exec("PRAGMA foreign_keys=OFF");
    exec("BEGIN IMMEDIATE");
    try {
        // Orphans would fail the new constraints, and they are already invisible in
        // the UI, so drop them before rebuilding.
        exec("DELETE FROM transactions WHERE account_id NOT IN (SELECT id FROM accounts)");
        exec("DELETE FROM transactions WHERE parent_id IS NOT NULL AND parent_id != ''"
             " AND parent_id NOT IN (SELECT id FROM transactions)");
        exec("DELETE FROM transaction_edits WHERE tx_id NOT IN (SELECT id FROM transactions)");
        exec("DELETE FROM savings_entries WHERE goal_id NOT IN (SELECT id FROM savings_goals)");

        // The old schema stored '' for "no parent" and "no bank id". Those are
        // values, not nulls, so they would not satisfy the new foreign key and
        // would collide in the partial unique index.
        exec("UPDATE transactions SET parent_id=NULL WHERE parent_id=''");
        exec("UPDATE transactions SET bank_tx_id=NULL WHERE bank_tx_id=''");

        // The unique index added below cannot be created while duplicates exist.
        // Duplicates are exactly the rows the old check-then-insert dedup let slip
        // through, so keep the earliest of each group.
        exec("DELETE FROM transactions WHERE bank_tx_id IS NOT NULL AND bank_tx_id != '' AND rowid NOT IN"
             " (SELECT MIN(rowid) FROM transactions WHERE bank_tx_id IS NOT NULL AND bank_tx_id != ''"
             "  GROUP BY account_id,bank_tx_id)");

        exec("ALTER TABLE transactions RENAME TO transactions_old");
        exec("ALTER TABLE transaction_edits RENAME TO transaction_edits_old");
        exec("ALTER TABLE savings_entries RENAME TO savings_entries_old");

        createSchema();

        exec("INSERT INTO transactions SELECT id,account_id,name,description,amount,currency,"
             "from_party,to_party,type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at"
             " FROM transactions_old");
        exec("INSERT INTO transaction_edits(id,tx_id,field,old_value,new_value,edited_at)"
             " SELECT id,tx_id,field,old_value,new_value,edited_at FROM transaction_edits_old");
        exec("INSERT INTO savings_entries(id,goal_id,year_month,planned,actual)"
             " SELECT id,goal_id,year_month,planned,actual FROM savings_entries_old");

        exec("DROP TABLE transactions_old");
        exec("DROP TABLE transaction_edits_old");
        exec("DROP TABLE savings_entries_old");
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        exec("PRAGMA foreign_keys=ON");
        throw;
    }
    exec("PRAGMA foreign_keys=ON");
}

void Database::createIndexes() const {
    // Enforces sync de-duplication in the database rather than relying on a
    // check-then-insert in application code, which two concurrent syncs can
    // interleave. Scoped per account because entry_reference is only unique there,
    // and partial because manual and split rows carry an empty bank_tx_id.
    exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_tx_bank_unique ON transactions(account_id,bank_tx_id)"
         " WHERE bank_tx_id IS NOT NULL AND bank_tx_id != ''");
    exec("CREATE INDEX IF NOT EXISTS idx_tx_date ON transactions(date DESC)");
    exec("CREATE INDEX IF NOT EXISTS idx_tx_account_date ON transactions(account_id,date DESC)");
    exec("CREATE INDEX IF NOT EXISTS idx_tx_parent ON transactions(parent_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_tx_category_date ON transactions(category,date)");
    exec("CREATE INDEX IF NOT EXISTS idx_tx_edits_tx ON transaction_edits(tx_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_savings_entries_goal ON savings_entries(goal_id)");
    exec("CREATE INDEX IF NOT EXISTS idx_budgets_month ON budgets(year_month)");
}

void Database::migrateToV3() const {
    if (!columnExists("accounts", "source")) {
        exec("ALTER TABLE accounts ADD COLUMN source TEXT DEFAULT 'manual'");
        exec("UPDATE accounts SET source='bank' WHERE bank_name IS NOT NULL AND bank_name != ''");
    }
    if (!columnExists("budgets", "notes")) {
        exec("ALTER TABLE budgets ADD COLUMN notes TEXT DEFAULT ''");
    }
}

void Database::init(){
    exec("CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY,value TEXT)");
    const int version = readSchemaVersion();
    if (version == 0) {
        createSchema();
    } else {
        if (version < 2) {
            std::cout << "[db] migrating schema from version " << version << " to 2" << std::endl;
            migrateToForeignKeys();
        }
        if (version < 3) {
            std::cout << "[db] migrating schema to version 3 (source + budget notes)" << std::endl;
            migrateToV3();
        }
    }
    createIndexes();
    writeSchemaVersion(kTargetSchemaVersion);
    seedCategories();
}

void Database::seedCategories() const {
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<Account> out;
    sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT id,name,type,currency,bank_name,iban,color,balance,"
        "COALESCE(NULLIF(source,''),'manual'),created_at,updated_at FROM accounts ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Account a;
        auto col=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        a.id=col(0);a.name=col(1);a.type=col(2);
        a.currency=sqlite3_column_text(st,3)?(const char*)sqlite3_column_text(st,3):"PLN";
        a.bankName=col(4);a.iban=col(5);a.color=col(6);
        a.balance=sqlite3_column_int64(st,7);
        a.source=col(8);a.createdAt=col(9);a.updatedAt=col(10);
        out.push_back(a);
    }
    sqlite3_finalize(st);return out;
}

Account Database::account(const std::string& id) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT id,name,type,currency,bank_name,iban,color,balance,"
        "COALESCE(NULLIF(source,''),'manual'),created_at,updated_at FROM accounts WHERE id=?");
    s.bindText(1, id);
    Account a;
    if (sqlite3_step(s.st) == SQLITE_ROW) {
        auto col=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        a.id=col(0);a.name=col(1);a.type=col(2);
        a.currency=col(3).empty()?"PLN":col(3);
        a.bankName=col(4);a.iban=col(5);a.color=col(6);
        a.balance=sqlite3_column_int64(s.st,7);
        a.source=col(8);a.createdAt=col(9);a.updatedAt=col(10);
    }
    return a;
}

void Database::upsertAccount(const Account& a){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    static const char* sql=
        "INSERT INTO accounts(id,name,type,currency,bank_name,iban,color,balance,source,created_at,updated_at)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET name=excluded.name,type=excluded.type,"
        "currency=excluded.currency,bank_name=excluded.bank_name,iban=excluded.iban,"
        "color=excluded.color,balance=excluded.balance,"
        "source=COALESCE(NULLIF(excluded.source,''),accounts.source),"
        "updated_at=excluded.updated_at";
    Stmt s(db_, sql);
    const std::string created=a.createdAt.empty()?now():a.createdAt;
    const std::string source=a.source.empty()?"manual":a.source;
    s.bindText(1,a.id);s.bindText(2,a.name);s.bindText(3,a.type);s.bindText(4,a.currency);
    s.bindText(5,a.bankName);s.bindText(6,a.iban);s.bindText(7,a.color);s.bindInt64(8,a.balance);
    s.bindText(9,source);s.bindText(10,created);s.bindText(11,now());
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("upsertAccount: ")+sqlite3_errmsg(db_));
}

void Database::recalcAccountBalanceLocked(const std::string& accountId) {
    // Top-level rows only — split children would double-count with the parent.
    Stmt sum(db_, "SELECT COALESCE(SUM(amount),0) FROM transactions WHERE account_id=?"
                   " AND (parent_id IS NULL OR parent_id='')");
    sum.bindText(1, accountId);
    int64_t balance = 0;
    if (sqlite3_step(sum.st) == SQLITE_ROW) balance = sqlite3_column_int64(sum.st, 0);
    Stmt upd(db_, "UPDATE accounts SET balance=?,updated_at=? WHERE id=?");
    upd.bindInt64(1, balance); upd.bindText(2, now()); upd.bindText(3, accountId);
    upd.step();
}

void Database::recalcAccountBalance(const std::string& accountId) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    recalcAccountBalanceLocked(accountId);
}

void Database::deleteAccount(const std::string& id){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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

TransactionPage Database::transactionPage(const std::string& acct,const std::string& from,const std::string& to,
                                          const std::string& category,const std::string& search,
                                          int limit,int offset)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    if (offset < 0) offset = 0;

    // Split parts are hidden from the list; they are returned nested under their
    // parent instead.
    std::string where=" WHERE (parent_id IS NULL OR parent_id='')";
    std::vector<std::string> args;
    if(!acct.empty()){where+=" AND account_id=?"; args.push_back(acct);}
    if(!from.empty()){where+=" AND date>=?"; args.push_back(from);}
    if(!to.empty()){where+=" AND date<=?"; args.push_back(to);}
    if(!category.empty()){where+=" AND category=?"; args.push_back(category);}
    if(!search.empty()){
        // LIKE is case-insensitive for ASCII in SQLite by default.
        where+=" AND (name LIKE ? OR description LIKE ? OR from_party LIKE ? OR to_party LIKE ?"
               " OR category LIKE ? OR tag LIKE ?)";
        const std::string pattern = "%" + search + "%";
        args.push_back(pattern);args.push_back(pattern);args.push_back(pattern);
        args.push_back(pattern);args.push_back(pattern);args.push_back(pattern);
    }

    TransactionPage page;
    {
        Stmt s(db_, "SELECT COUNT(*) FROM transactions" + where);
        for(std::size_t i=0;i<args.size();++i) s.bindText(static_cast<int>(i)+1,args[i]);
        if(sqlite3_step(s.st)==SQLITE_ROW) page.total=sqlite3_column_int64(s.st,0);
    }
    {
        Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
            "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions" + where +
            " ORDER BY date DESC,id DESC LIMIT ? OFFSET ?");
        int bind=1;
        for(const std::string& arg:args) s.bindText(bind++,arg);
        s.bindInt(bind++,limit);
        s.bindInt(bind,offset);
        while(sqlite3_step(s.st)==SQLITE_ROW)page.items.push_back(readTx(s.st));
    }
    return page;
}

Transaction Database::transaction(const std::string& id)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE id=?");
    s.bindText(1, id);
    Transaction t;if(sqlite3_step(s.st)==SQLITE_ROW)t=readTx(s.st);
    return t;
}

void Database::insertTx(const Transaction& t){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    s.bindTextOrNull(14,t.bankTxId);s.bindTextOrNull(15,t.parentId);s.bindText(16,created);s.bindText(17,updated);
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("insertTx: ")+sqlite3_errmsg(db_));
    Account acc = account(t.accountId);
    if (acc.source != "bank") recalcAccountBalanceLocked(t.accountId);
}

void Database::updateTx(const std::string& id,const Transaction& u){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    check("currency",old.currency,u.currency);
    check("category",old.category,u.category);check("tag",old.tag,u.tag);
    check("from_party",old.fromParty,u.fromParty);check("to_party",old.toParty,u.toParty);
    check("type",old.type,u.type);check("date",old.date,u.date);
    Stmt s(db_, "UPDATE transactions SET name=?,description=?,amount=?,currency=?,category=?,tag=?,"
        "from_party=?,to_party=?,type=?,date=?,updated_at=? WHERE id=?");
    s.bindText(1,u.name);s.bindText(2,u.description);s.bindInt64(3,u.amount);s.bindText(4,u.currency);
    s.bindText(5,u.category);s.bindText(6,u.tag);s.bindText(7,u.fromParty);s.bindText(8,u.toParty);
    s.bindText(9,u.type);s.bindText(10,u.date);s.bindText(11,now());s.bindText(12,id);
    if(s.step()!=SQLITE_DONE)throw std::runtime_error(std::string("updateTx: ")+sqlite3_errmsg(db_));
    Account acc = account(old.accountId);
    if (acc.source != "bank") recalcAccountBalanceLocked(old.accountId);
}

DeleteResult Database::deleteTx(const std::string& id){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    const Transaction existing = transaction(id);
    if (existing.id.empty()) {
        return DeleteResult::NotFound;
    }
    if (existing.source != "manual") {
        // The old code issued a DELETE guarded by source='manual' and reported
        // success either way, so deleting a bank transaction looked like it worked
        // until the row reappeared on the next refresh.
        return DeleteResult::NotDeletable;
    }
    const std::string accountId = existing.accountId;
    // Splits and edit history go with it via ON DELETE CASCADE.
    Stmt s(db_, "DELETE FROM transactions WHERE id=?");
    s.bindText(1,id);
    if (s.step()!=SQLITE_DONE) throw std::runtime_error(std::string("deleteTx: ")+sqlite3_errmsg(db_));
    Account acc = account(accountId);
    if (acc.source != "bank") recalcAccountBalanceLocked(accountId);
    return DeleteResult::Deleted;
}

Transaction Database::transactionByBankId(const std::string& accountId, const std::string& bankTxId) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at"
        " FROM transactions WHERE account_id=? AND bank_tx_id=?");
    s.bindText(1, accountId);
    s.bindText(2, bankTxId);
    Transaction t;
    if (sqlite3_step(s.st) == SQLITE_ROW) t = readTx(s.st);
    return t;
}

bool Database::fieldWasUserEdited(const std::string& txId, const std::string& field) const {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT 1 FROM transaction_edits WHERE tx_id=? AND field=? LIMIT 1");
    s.bindText(1, txId);
    s.bindText(2, field);
    return sqlite3_step(s.st) == SQLITE_ROW;
}

bool Database::txExists(const std::string& accountId,const std::string& bankTxId)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT 1 FROM transactions WHERE account_id=? AND bank_tx_id=?");
    s.bindText(1, accountId);
    s.bindText(2, bankTxId);
    return sqlite3_step(s.st)==SQLITE_ROW;
}

std::vector<std::string> Database::existingBankTxIds(const std::string& accountId)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT bank_tx_id FROM transactions WHERE account_id=? AND bank_tx_id IS NOT NULL AND bank_tx_id != ''");
    s.bindText(1, accountId);
    std::vector<std::string> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        const unsigned char* p=sqlite3_column_text(s.st,0);
        if(p) out.emplace_back(reinterpret_cast<const char*>(p));
    }
    return out;
}

std::vector<Transaction> Database::subTx(const std::string& pid)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE parent_id=?");
    s.bindText(1, pid);
    std::vector<Transaction> out;
    while(sqlite3_step(s.st)==SQLITE_ROW)out.push_back(readTx(s.st));
    return out;
}

void Database::splitTx(const std::string& pid,const std::vector<Transaction>& parts){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    const Transaction parent = transaction(pid);
    if (parent.id.empty()) {
        throw std::invalid_argument("transaction not found");
    }
    if (parts.empty()) {
        throw std::invalid_argument("a split needs at least one part");
    }
    if (!parent.parentId.empty()) {
        throw std::invalid_argument("a split part cannot itself be split");
    }
    // Reporting reads the parts instead of the parent, so parts that do not add up
    // to the parent would quietly change every total the transaction feeds into.
    int64_t sum = 0;
    for (const Transaction& part : parts) {
        sum += part.amount;
    }
    if (sum != parent.amount) {
        throw std::invalid_argument("split parts sum to " + std::to_string(sum) +
                                    " but the transaction is " + std::to_string(parent.amount));
    }

    exec("BEGIN IMMEDIATE");
    try {
        { Stmt s(db_, "DELETE FROM transactions WHERE parent_id=?"); s.bindText(1,pid); s.step(); }
        for(auto& p:parts){Transaction t=p;t.parentId=pid;if(t.id.empty())t.id=uuid();insertTx(t);}
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    Account acc = account(parent.accountId);
    if (acc.source != "bank") recalcAccountBalanceLocked(parent.accountId);
}

std::vector<Transaction> Database::subTxForParents(const std::vector<std::string>& parentIds)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<Transaction> out;
    if (parentIds.empty()) return out;
    std::string sql="SELECT id,account_id,name,description,amount,currency,from_party,to_party,"
        "type,category,tag,date,source,bank_tx_id,parent_id,created_at,updated_at FROM transactions WHERE parent_id IN (";
    for(std::size_t i=0;i<parentIds.size();++i) sql += i ? ",?" : "?";
    sql += ")";
    Stmt s(db_, sql);
    for(std::size_t i=0;i<parentIds.size();++i) s.bindText(static_cast<int>(i)+1, parentIds[i]);
    while(sqlite3_step(s.st)==SQLITE_ROW)out.push_back(readTx(s.st));
    return out;
}

std::vector<std::pair<std::string,int64_t>> Database::editCounts(const std::vector<std::string>& txIds)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<std::pair<std::string,int64_t>> out;
    if (txIds.empty()) return out;
    std::string sql="SELECT tx_id,COUNT(*) FROM transaction_edits WHERE tx_id IN (";
    for(std::size_t i=0;i<txIds.size();++i) sql += i ? ",?" : "?";
    sql += ") GROUP BY tx_id";
    Stmt s(db_, sql);
    for(std::size_t i=0;i<txIds.size();++i) s.bindText(static_cast<int>(i)+1, txIds[i]);
    while(sqlite3_step(s.st)==SQLITE_ROW){
        const unsigned char* p=sqlite3_column_text(s.st,0);
        out.emplace_back(p?reinterpret_cast<const char*>(p):"", sqlite3_column_int64(s.st,1));
    }
    return out;
}

std::vector<TxEdit> Database::txHistory(const std::string& tid)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::vector<Category> out;sqlite3_stmt*st;
    sqlite3_prepare_v2(db_,"SELECT name,icon,color FROM categories ORDER BY name",-1,&st,nullptr);
    while(sqlite3_step(st)==SQLITE_ROW){
        Category c;auto col=[&](int i)->std::string{auto p=sqlite3_column_text(st,i);return p?(const char*)p:"";};
        c.name=col(0);c.icon=col(1);c.color=col(2);out.push_back(c);
    }
    sqlite3_finalize(st);return out;
}
void Database::upsertCategory(const Category& c){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO categories(name,icon,color)VALUES(?,?,?) ON CONFLICT(name) DO UPDATE SET icon=excluded.icon,color=excluded.color");
    s.bindText(1,c.name);s.bindText(2,c.icon);s.bindText(3,c.color);
    s.step();
}

// ===== BUDGETS =====
std::vector<Budget> Database::budgets(const std::string& ym)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "SELECT id,year_month,category,planned,COALESCE(notes,'') FROM budgets WHERE year_month=?");
    s.bindText(1, ym);
    std::vector<Budget> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        Budget b;b.id=sqlite3_column_int64(s.st,0);
        auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        b.yearMonth=c(1);b.category=c(2);b.planned=sqlite3_column_int64(s.st,3);b.notes=c(4);out.push_back(b);
    }
    return out;
}
void Database::upsertBudget(const Budget& b){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO budgets(year_month,category,planned,notes)VALUES(?,?,?,?)"
        " ON CONFLICT(year_month,category) DO UPDATE SET planned=excluded.planned,notes=excluded.notes");
    s.bindText(1,b.yearMonth);s.bindText(2,b.category);s.bindInt64(3,b.planned);s.bindText(4,b.notes);
    s.step();
}
std::vector<BudgetLine> Database::budgetSummary(const std::string& ym)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    // A transaction that has been split is excluded and its parts are counted
    // instead; counting both sides double-counted every split.
    Stmt s(db_, "SELECT b.category,b.planned,COALESCE(SUM(ABS(t.amount)),0),COALESCE(b.notes,'') FROM budgets b "
        "LEFT JOIN transactions t ON t.category=b.category AND t.type='expense' AND substr(t.date,1,7)=b.year_month "
        "AND NOT EXISTS(SELECT 1 FROM transactions c WHERE c.parent_id=t.id) "
        "WHERE b.year_month=? GROUP BY b.category");
    s.bindText(1, ym);
    std::vector<BudgetLine> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        BudgetLine l;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        l.category=c(0);l.planned=sqlite3_column_int64(s.st,1);l.actual=sqlite3_column_int64(s.st,2);
        l.notes=c(3);out.push_back(l);
    }
    return out;
}
int Database::copyBudgets(const std::string& fromYm, const std::string& toYm) {
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    if (fromYm.empty() || toYm.empty() || fromYm == toYm) return 0;
    auto rows = budgets(fromYm);
    int n = 0;
    for (auto b : rows) {
        b.yearMonth = toYm;
        upsertBudget(b);
        ++n;
    }
    return n;
}

// ===== TODOS =====
std::vector<Todo> Database::todos()const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO todos(name,description,amount,due_date,done,created_at)VALUES(?,?,?,?,0,?)");
    s.bindText(1,t.name);s.bindText(2,t.description);s.bindInt64(3,t.amount);s.bindText(4,t.dueDate);s.bindText(5,now());
    s.step();
    return sqlite3_last_insert_rowid(db_);
}
void Database::updateTodo(int64_t id,const Todo& t){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "UPDATE todos SET name=?,description=?,amount=?,due_date=?,done=? WHERE id=?");
    s.bindText(1,t.name);s.bindText(2,t.description);s.bindInt64(3,t.amount);s.bindText(4,t.dueDate);
    s.bindInt(5,t.done?1:0);s.bindInt64(6,id);
    s.step();
}
void Database::deleteTodo(int64_t id){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "DELETE FROM todos WHERE id=?");
    s.bindInt64(1, id);
    s.step();
}

// ===== SAVINGS =====
std::vector<SavingsGoal> Database::savingsGoals()const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO savings_goals(name,target,deadline,created_at)VALUES(?,?,?,?)");
    s.bindText(1,g.name);s.bindInt64(2,g.target);s.bindText(3,g.deadline);s.bindText(4,now());
    s.step();
    return sqlite3_last_insert_rowid(db_);
}
void Database::deleteGoal(int64_t id){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    { Stmt s(db_, "DELETE FROM savings_entries WHERE goal_id=?"); s.bindInt64(1,id); s.step(); }
    { Stmt s(db_, "DELETE FROM savings_goals WHERE id=?"); s.bindInt64(1,id); s.step(); }
}
void Database::upsertEntry(const SavingsEntry& e){
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO savings_entries(goal_id,year_month,planned,actual)VALUES(?,?,?,?)"
        " ON CONFLICT(goal_id,year_month) DO UPDATE SET planned=excluded.planned,actual=excluded.actual");
    s.bindInt64(1,e.goalId);s.bindText(2,e.yearMonth);s.bindInt64(3,e.planned);s.bindInt64(4,e.actual);
    s.step();
}
std::vector<SavingsEntry> Database::savingsEntries(int64_t gid)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    Stmt s(db_, "INSERT INTO sync_history(synced_at,bank_name,new_tx_count,details)VALUES(?,?,?,?)");
    s.bindText(1,r.syncedAt.empty()?now():r.syncedAt);s.bindText(2,r.bankName);s.bindInt(3,r.newTx);s.bindText(4,r.details);
    s.step();
}
std::vector<SyncRec> Database::syncHistory(int lim)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
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
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    // Grouped by currency as well: summing minor units across currencies would add
    // grosze to cents and produce a meaningless figure.
    std::string sql="SELECT category,substr(date,1,7) as ym,COALESCE(NULLIF(currency,''),'PLN') as cur,"
        "SUM(ABS(amount)) FROM transactions t WHERE type='expense'"
        " AND NOT EXISTS(SELECT 1 FROM transactions c WHERE c.parent_id=t.id)";
    int nextBind=1;
    if(!from.empty()){sql+=" AND date>=?"; }
    if(!to.empty()){sql+=" AND date<=?"; }
    sql+=" GROUP BY category,ym,cur ORDER BY ym,category,cur";
    Stmt s(db_, sql);
    if(!from.empty()) s.bindText(nextBind++, from);
    if(!to.empty()) s.bindText(nextBind++, to);
    std::vector<StatsRow> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        StatsRow r;auto c=[&](int i)->std::string{auto p=sqlite3_column_text(s.st,i);return p?(const char*)p:"";};
        r.category=c(0);r.yearMonth=c(1);r.currency=c(2);r.total=sqlite3_column_int64(s.st,3);out.push_back(r);
    }
    return out;
}

std::vector<MonthTotals> Database::monthTotals(const std::string& from,const std::string& to)const{
    std::lock_guard<std::recursive_mutex> lk(mutex_);
    std::string sql=
        "SELECT COALESCE(NULLIF(currency,''),'PLN'),"
        " COALESCE(SUM(CASE WHEN type='income' THEN ABS(amount) ELSE 0 END),0),"
        " COALESCE(SUM(CASE WHEN type='expense' THEN ABS(amount) ELSE 0 END),0)"
        " FROM transactions t"
        " WHERE (parent_id IS NULL OR parent_id='')"
        " AND NOT EXISTS(SELECT 1 FROM transactions c WHERE c.parent_id=t.id)";
    if(!from.empty()) sql += " AND date>=?";
    if(!to.empty()) sql += " AND date<=?";
    sql += " GROUP BY 1 ORDER BY 1";
    Stmt s(db_, sql);
    int bind = 1;
    if(!from.empty()) s.bindText(bind++, from);
    if(!to.empty()) s.bindText(bind++, to);
    std::vector<MonthTotals> out;
    while(sqlite3_step(s.st)==SQLITE_ROW){
        MonthTotals m;
        auto p=sqlite3_column_text(s.st,0);
        m.currency = p ? reinterpret_cast<const char*>(p) : "PLN";
        m.income = sqlite3_column_int64(s.st,1);
        m.expense = sqlite3_column_int64(s.st,2);
        out.push_back(m);
    }
    return out;
}

} // namespace db
