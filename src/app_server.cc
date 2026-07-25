#include "app_server.h"
#include <locale>

#include "json_mapper.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/sha.h>

namespace {
std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string toLowerCopy(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

/// Keyword → category for bank sync. First match wins; keep patterns specific.
std::string inferCategory(const std::string& name, const std::string& description, const std::string& type) {
    if (type == "income") return "income";
    if (type == "transfer") return "transfer";
    const std::string hay = toLowerCopy(name + " " + description);
    struct Rule { const char* needle; const char* category; };
    static const Rule rules[] = {
        {"biedronka", "food"}, {"lidl", "food"}, {"zabka", "food"}, {"żabka", "food"},
        {"auchan", "food"}, {"carrefour", "food"}, {"netto", "food"},
        {"uber", "transport"}, {"bolt", "transport"}, {"orlen", "transport"}, {"bp ", "transport"},
        {"shell", "transport"}, {"pkp", "transport"}, {"flixbus", "transport"},
        {"netflix", "entertainment"}, {"spotify", "entertainment"}, {"hbo", "entertainment"},
        {"cinema", "entertainment"}, {"multikino", "entertainment"},
        {"enea", "utilities"}, {"pge", "utilities"}, {"tauron", "utilities"}, {"orange", "utilities"},
        {"play ", "utilities"}, {"t-mobile", "utilities"}, {"upc", "utilities"}, {"vectra", "utilities"},
        {"apteka", "health"}, {"medicover", "health"}, {"luxmed", "health"}, {"szpital", "health"},
        {"allegro", "shopping"}, {"amazon", "shopping"}, {"zalando", "shopping"}, {"ikea", "shopping"},
        {"hebe", "shopping"}, {"rossmann", "shopping"},
        {"piwo", "alko"}, {"alkohol", "alko"}, {"liquor", "alko"},
        {"booking", "wyjazdy"}, {"airbnb", "wyjazdy"}, {"ryanair", "wyjazdy"}, {"hotel", "wyjazdy"},
        {"oszczęd", "savings"}, {"lokata", "savings"},
        {nullptr, nullptr}
    };
    for (int i = 0; rules[i].needle; ++i) {
        if (hay.find(rules[i].needle) != std::string::npos) return rules[i].category;
    }
    return "other";
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}
}

AppServer::AppServer(EnableBankingConfig config)
        : config_(std::move(config)), client_(config_), running_(false) {}

AppServer::~AppServer() {
    running_ = false;
    syncWake_.notify_all();
    if (syncThread_.joinable()) {
        syncThread_.join();
    }
}

void AppServer::run() {
    int port = 8080;
    const char* portEnv = std::getenv("PORT");
    if (portEnv != nullptr && *portEnv != '\0') {
        // std::stoi would throw std::invalid_argument for a non-numeric PORT and
        // silently accept out-of-range values such as "99999".
        errno = 0;
        char* parseEnd = nullptr;
        const long parsed = std::strtol(portEnv, &parseEnd, 10);
        if (parseEnd == portEnv || *parseEnd != '\0' || errno != 0 || parsed < 1 || parsed > 65535) {
            throw std::runtime_error(std::string("PORT must be an integer in 1..65535, got: ") + portEnv);
        }
        port = static_cast<int>(parsed);
    }
    if (!config_.mockMode) {
        if (config_.redirectUri.empty()) {
            throw std::runtime_error("ENABLEBANKING_REDIRECT_URI or PUBLIC_BASE_URL is required");
        }
        if (config_.apiToken.empty()) {
            std::cout << "Warning: ENABLEBANKING_API_TOKEN is not set; /api/* endpoints will be disabled." << std::endl;
        }
        if (config_.accessToken.empty() && (config_.privateKeyPem.empty() || config_.appCode.empty())) {
            throw std::runtime_error("Set ENABLEBANKING_ACCESS_TOKEN or ENABLEBANKING_PRIVATE_KEY_PATH and ENABLEBANKING_APP_CODE");
        }
    }
    if (config_.aspspName.empty() || config_.aspspCountry.empty()) {
        std::cout << "Warning: ENABLEBANKING_ASPSP_NAME / ENABLEBANKING_ASPSP_COUNTRY not set; /start-auth will fail." << std::endl;
    }

    std::cout << "Starting service on port " << port << std::endl;
    std::cout << "Callback URL: " << config_.redirectUri << std::endl;

    // Initialize SQLite database
    if (!config_.dataDir.empty()) {
        // Falling back to an in-memory database here would silently discard every
        // transaction the user edits, so an unusable data directory is fatal.
        std::error_code dirError;
        std::filesystem::create_directories(config_.dataDir, dirError);
        if (dirError) {
            throw std::runtime_error("ENABLEBANKING_DATA_DIR is not usable (" + config_.dataDir +
                                     "): " + dirError.message());
        }
        const std::string probePath = config_.dataDir + "/.write-probe";
        std::ofstream probe(probePath);
        if (!probe) {
            throw std::runtime_error("ENABLEBANKING_DATA_DIR is not writable: " + config_.dataDir);
        }
        probe.close();
        std::filesystem::remove(probePath, dirError);

        const std::string dbPath = config_.dataDir + "/banksconnect.db";
        db_ = std::make_unique<db::Database>(dbPath);
        std::cout << "Database: " << dbPath << std::endl;
    } else {
        db_ = std::make_unique<db::Database>(":memory:");
        std::cout << "Database: in-memory (set ENABLEBANKING_DATA_DIR for persistence)" << std::endl;
    }

    if (config_.mockMode) {
        // Seed mock budgets for the current month
        std::string ym = db::Database::now().substr(0, 7);
        db::Budget b1; b1.yearMonth=ym; b1.category="food"; b1.planned=150000; db_->upsertBudget(b1);
        db::Budget b2; b2.yearMonth=ym; b2.category="transport"; b2.planned=30000; db_->upsertBudget(b2);
        db::Budget b3; b3.yearMonth=ym; b3.category="entertainment"; b3.planned=20000; db_->upsertBudget(b3);
        
        // Seed mock savings goal
        db::SavingsGoal sg; sg.name="Nowy Samochód"; sg.target=6000000; sg.deadline="2027-01-31";
        int64_t gid = db_->insertGoal(sg);
        for(int m=1; m<=12; ++m) {
            char buf[10]; snprintf(buf, sizeof(buf), "2026-%02d", m);
            db::SavingsEntry se; se.goalId=gid; se.yearMonth=buf; 
            se.planned = m * 500000;
            se.actual = (m <= 7) ? (m * 480000 + (m % 2)*15000) : 0; // filled up to month 7
            db_->upsertEntry(se);
        }
    }

    // Load persisted sessions from disk
    loadSessions();

    running_ = true;
    startSyncLoop();
    serve(port);
}

void AppServer::startSyncLoop() {
    // The thread starts even when polling is disabled, because manual and webhook
    // triggered syncs are dispatched onto it too.
    syncThread_ = std::thread([this]() {
        std::string reason = config_.syncIntervalSeconds > 0 ? "startup" : "";
        while (running_) {
            if (!reason.empty()) {
                syncInProgress_ = true;
                try {
                    syncOnce(reason);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    lastError_ = error.what();
                }
                syncInProgress_ = false;
            }

            std::unique_lock<std::mutex> lock(syncWakeMutex_);
            const auto wakeup = [this]() { return syncRequested_ || !running_; };
            if (config_.syncIntervalSeconds > 0) {
                syncWake_.wait_for(lock, std::chrono::seconds(config_.syncIntervalSeconds), wakeup);
            } else {
                syncWake_.wait(lock, wakeup);
            }
            if (!running_) {
                break;
            }
            if (syncRequested_) {
                reason = syncRequestReason_.empty() ? "manual" : syncRequestReason_;
                syncRequested_ = false;
                syncRequestReason_.clear();
            } else {
                reason = "poll";
            }
        }
    });
}

void AppServer::requestSync(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(syncWakeMutex_);
        syncRequested_ = true;
        syncRequestReason_ = reason;
    }
    syncWake_.notify_all();
}

void AppServer::syncOnce(const std::string& reason) {
    // Snapshot the sessions so each account keeps its owning bank's identity.
    struct AccountRef {
        std::string accountId;
        std::string bankName;
        std::string bankCountry;
    };
    std::vector<AccountRef> refs;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (const auto& session : bankSessions_) {
            for (const auto& id : session.accountIds) {
                refs.push_back({id, session.aspspName, session.aspspCountry});
            }
        }
    }

    std::vector<acc> accounts;
    std::vector<trans> transactions;
    std::vector<db::Account> dbAccounts;
    std::vector<db::Transaction> dbTransactions;
    std::ostringstream summary;
    summary << "reason=" << reason;
    std::vector<std::string> upstreamFailures;

    auto lastFour = [](const std::string& iban) -> std::string {
        if (iban.size() <= 4) return iban;
        return iban.substr(iban.size() - 4);
    };

    // An error body parses to a balance of zero and an empty transaction list, so
    // every upstream status is checked before its payload is allowed to replace
    // stored data.
    auto succeeded = [](long statusCode) { return statusCode >= 200 && statusCode < 300; };

    // Balances already on record, used to keep the last known good figure when the
    // balance call fails.
    std::map<std::string, int64_t> storedBalances;
    if (db_) {
        for (const auto& stored : db_->accounts()) {
            storedBalances[stored.id] = stored.balance;
        }
    }

    if (!refs.empty()) {
        // Proper per-account Enable Banking endpoints. Each account is fully
        // resolved (details + balances + transactions) so we can attach the
        // right bank name, currency and transactions to it.
        for (const AccountRef& ref : refs) {
            const std::string& accountId = ref.accountId;
            const ::HttpResponse detailsResp = client_.getAccountDetails(accountId);
            const ::HttpResponse balResp = client_.getAccountBalances(accountId);
            const ::HttpResponse txResp = client_.getAccountTransactions(accountId);

            summary << " account=" << accountId
                    << " det_status=" << detailsResp.statusCode
                    << " bal_status=" << balResp.statusCode
                    << " tx_status=" << txResp.statusCode;

            const bool detailsOk = succeeded(detailsResp.statusCode);
            const bool balanceOk = succeeded(balResp.statusCode);
            const bool txOk = succeeded(txResp.statusCode);
            if (!detailsOk || !balanceOk || !txOk) {
                std::ostringstream failure;
                failure << "account " << accountId << ":";
                if (!detailsOk) failure << " details=" << detailsResp.statusCode;
                if (!balanceOk) failure << " balances=" << balResp.statusCode;
                if (!txOk) failure << " transactions=" << txResp.statusCode;
                upstreamFailures.push_back(failure.str());
            }

            const BankAccountDetails details = detailsOk ? parseAccountDetails(detailsResp.body) : BankAccountDetails{};
            const BankBalance balance = balanceOk ? parseBalance(balResp.body) : BankBalance{};

            std::string currency = !details.currency.empty() ? details.currency : balance.currency;
            if (currency.empty()) {
                currency = "PLN";
            }

            // Build a human-friendly account name (never a bare UUID).
            std::string friendlyName = details.name;
            if (friendlyName.empty()) {
                friendlyName = ref.bankName;
                if (!details.iban.empty()) {
                    friendlyName += " ••" + lastFour(details.iban);
                }
            }
            if (friendlyName.empty()) {
                friendlyName = "Bank Account";
            }

            const std::string dbAccountId = "bank_acc_" + accountId;

            // Keep the last known balance rather than zeroing the account out.
            int64_t effectiveBalance = balance.minorUnits;
            if (!balanceOk) {
                const auto stored = storedBalances.find(dbAccountId);
                effectiveBalance = stored != storedBalances.end() ? stored->second : 0;
            }

            db::Account dba;
            dba.id = dbAccountId;
            dba.name = friendlyName;
            dba.type = "bank";
            dba.currency = currency;
            dba.bankName = ref.bankName;
            dba.iban = details.iban;
            dba.balance = effectiveBalance;
            dba.source = "bank";
            dbAccounts.push_back(dba);

            // Keep an in-memory representation for /status.
            accounts.emplace_back(friendlyName, effectiveBalance);

            // A failed transactions call yields no rows; treating that as "the bank
            // returned nothing" is fine because inserts are additive, but parsing an
            // error body could otherwise create junk rows.
            const std::vector<trans> acctTx = txOk ? parseTransactions(txResp.body) : std::vector<trans>{};
            int accountTxCount = 0;
            for (const trans& t : acctTx) {
                accounts.back().addTransaction(t);
                transactions.push_back(t);

                db::Transaction dbt;
                dbt.accountId = dbAccountId;
                dbt.name = t.name;
                dbt.description = t.opis;
                dbt.amount = t.amount;
                dbt.currency = !t.currencyCode.empty() ? t.currencyCode : currency;
                dbt.fromParty = t.from;
                dbt.toParty = t.to;
                dbt.type = enumToString(t.type);
                dbt.category = inferCategory(t.name, t.opis, dbt.type);
                dbt.date = t.date;
                dbt.source = "bank";
                // De-duplicate on the real bank id; fall back to a per-account
                // composite key (never just name+date) so distinct transactions
                // on the same day are not collapsed.
                if (!t.bankTxId.empty()) {
                    dbt.bankTxId = accountId + ":" + t.bankTxId;
                } else {
                    dbt.bankTxId = accountId + ":" + t.date + ":" + std::to_string(t.amount) +
                                   ":" + std::to_string(accountTxCount);
                }
                dbTransactions.push_back(dbt);
                ++accountTxCount;
            }
        }
    } else {
        // Fallback to legacy generic endpoints (before a session is established).
        const ::HttpResponse accountsResponse = client_.getAccounts();
        const ::HttpResponse balancesResponse = client_.getBalances();
        const ::HttpResponse transactionsResponse = client_.getTransactions();

        if (!succeeded(accountsResponse.statusCode)) {
            // Without a usable accounts response there is nothing to reconcile
            // against, and continuing would overwrite good data with parsed noise.
            throw std::runtime_error("upstream /accounts returned status " +
                                     std::to_string(accountsResponse.statusCode));
        }
        if (!succeeded(balancesResponse.statusCode) || !succeeded(transactionsResponse.statusCode)) {
            upstreamFailures.push_back("generic endpoints: balances=" +
                                       std::to_string(balancesResponse.statusCode) + " transactions=" +
                                       std::to_string(transactionsResponse.statusCode));
        }

        accounts = parseAccounts(accountsResponse.body);
        transactions = succeeded(transactionsResponse.statusCode) ? parseTransactions(transactionsResponse.body)
                                                                 : std::vector<trans>{};
        BankBalance genericBalance = succeeded(balancesResponse.statusCode) ? parseBalance(balancesResponse.body)
                                                                           : BankBalance{};

        const std::string fallbackAccountId = accounts.empty() ? "unknown" : accounts.front().getName();
        const std::string dbAccountId = "bank_acc_" + fallbackAccountId;
        for (auto& a : accounts) {
            db::Account dba;
            dba.id = "bank_acc_" + a.getName();
            dba.name = a.getName();
            dba.type = "bank";
            dba.currency = "PLN";
            
            // Try extracting bankName from generic JSON body
            std::size_t nPos = accountsResponse.body.find("\"bank_name\"");
            if (nPos == std::string::npos) nPos = accountsResponse.body.find("\"bankName\"");
            if (nPos != std::string::npos) {
                std::size_t col = accountsResponse.body.find(':', nPos);
                std::size_t val = accountsResponse.body.find('"', col);
                if (val != std::string::npos) {
                    std::size_t end = accountsResponse.body.find('"', val + 1);
                    if (end != std::string::npos) dba.bankName = accountsResponse.body.substr(val + 1, end - val - 1);
                }
            }
            if (dba.bankName.empty()) dba.bankName = "Enable Banking Proxy";
            
            dba.balance = a.getBalance();
            if (dba.balance == 0 && genericBalance.found) {
                dba.balance = genericBalance.minorUnits;
                a.setBalance(genericBalance.minorUnits);
            }
            dba.source = "bank";
            dbAccounts.push_back(dba);
        }
        int idx = 0;
        for (const trans& t : transactions) {
            if (!accounts.empty()) {
                accounts.front().addTransaction(t);
            }
            db::Transaction dbt;
            dbt.accountId = dbAccountId;
            dbt.name = t.name;
            dbt.description = t.opis;
            dbt.amount = t.amount;
            dbt.currency = !t.currencyCode.empty() ? t.currencyCode : "PLN";
            dbt.fromParty = t.from;
            dbt.toParty = t.to;
            dbt.type = enumToString(t.type);
            dbt.category = inferCategory(t.name, t.opis, dbt.type);
            dbt.date = t.date;
            dbt.source = "bank";
            dbt.bankTxId = !t.bankTxId.empty()
                    ? fallbackAccountId + ":" + t.bankTxId
                    : fallbackAccountId + ":" + t.date + ":" + std::to_string(t.amount) + ":" + std::to_string(idx);
            dbTransactions.push_back(dbt);
            ++idx;
        }

        summary << " accounts_status=" << accountsResponse.statusCode
                << " balances_status=" << balancesResponse.statusCode
                << " transactions_status=" << transactionsResponse.statusCode;
    }

    summary << " accounts=" << accounts.size()
            << " transactions=" << transactions.size();

    if (db_) {
        for (const auto& dba : dbAccounts) {
            db_->upsertAccount(dba);
        }
        int newTxCount = 0;
        int updatedTxCount = 0;
        // Ids already stored are fetched once per account. Re-synced rows are
        // updated (with edit history) instead of skipped, so bank corrections land.
        std::map<std::string, std::set<std::string>> seenByAccount;
        for (auto& dbt : dbTransactions) {
            auto known = seenByAccount.find(dbt.accountId);
            if (known == seenByAccount.end()) {
                const std::vector<std::string> stored = db_->existingBankTxIds(dbt.accountId);
                known = seenByAccount.emplace(dbt.accountId,
                                              std::set<std::string>(stored.begin(), stored.end())).first;
            }
            if (!known->second.insert(dbt.bankTxId).second) {
                db::Transaction existing = db_->transactionByBankId(dbt.accountId, dbt.bankTxId);
                if (existing.id.empty()) continue; // duplicate inside this batch
                // Preserve user-edited fields (especially category/tag).
                db::Transaction merged = existing;
                merged.name = dbt.name;
                merged.description = dbt.description;
                merged.amount = dbt.amount;
                merged.currency = dbt.currency;
                merged.fromParty = dbt.fromParty;
                merged.toParty = dbt.toParty;
                merged.type = dbt.type;
                merged.date = dbt.date;
                if (!db_->fieldWasUserEdited(existing.id, "category")) {
                    merged.category = dbt.category;
                }
                if (!db_->fieldWasUserEdited(existing.id, "tag")) {
                    merged.tag = existing.tag.empty() ? dbt.tag : existing.tag;
                }
                db_->updateTx(existing.id, merged);
                ++updatedTxCount;
                continue;
            }
            dbt.id = db_->uuid();
            db_->insertTx(dbt);
            ++newTxCount;
        }
        if (updatedTxCount > 0) {
            summary << " updated_tx=" << updatedTxCount;
        }
        db::SyncRec rec;
        rec.syncedAt = db::Database::now();
        rec.bankName = "All Banks Sync";
        rec.newTx = newTxCount;
        rec.details = summary.str();
        db_->recordSync(rec);
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    accounts_ = std::move(accounts);
    transactions_ = std::move(transactions);
    lastSyncSummary_ = summary.str();
    lastSyncTime_ = db::Database::now();
    // A partial sync must not look like a clean one, or the app will keep showing
    // stale figures with no indication that a bank call failed.
    if (upstreamFailures.empty()) {
        lastError_.clear();
    } else {
        std::ostringstream error;
        error << "some bank data could not be refreshed (";
        for (std::size_t i = 0; i < upstreamFailures.size(); ++i) {
            if (i > 0) error << "; ";
            error << upstreamFailures[i];
        }
        error << ")";
        lastError_ = error.str();
    }

    if (config_.debugMode) {
        std::cout << "[DEBUG] Sync completed: " << lastSyncSummary_ << std::endl;
    }
}

void AppServer::saveSessions() const {
    if (config_.dataDir.empty()) {
        return;
    }
    const std::string filePath = config_.dataDir + "/sessions.json";
    std::ostringstream out;
    out << "{\"sessions\":[";
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        for (std::size_t i = 0; i < bankSessions_.size(); ++i) {
            const auto& s = bankSessions_[i];
            if (i > 0) out << ",";
            out << "{\"aspsp_name\":" << jsonString(s.aspspName)
                << ",\"aspsp_country\":" << jsonString(s.aspspCountry)
                << ",\"session_id\":" << jsonString(s.sessionId)
                << ",\"authorization_id\":" << jsonString(s.authorizationId)
                << ",\"account_ids\":[";
            for (std::size_t j = 0; j < s.accountIds.size(); ++j) {
                if (j > 0) out << ",";
                out << jsonString(s.accountIds[j]);
            }
            out << "]}";
        }
    }
    out << "]}";

    // This file holds bank session IDs, which are bearer-equivalent credentials.
    // Write to a private temp file and rename, so a crash mid-write cannot leave a
    // truncated file behind and the contents are never briefly world-readable.
    const std::string tempPath = filePath + ".tmp";
    const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        std::cerr << "[EB] Failed to save sessions to " << filePath << ": " << std::strerror(errno) << std::endl;
        return;
    }
    const std::string payload = out.str();
    std::size_t written = 0;
    bool writeOk = true;
    while (written < payload.size()) {
        const ssize_t n = ::write(fd, payload.data() + written, payload.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            writeOk = false;
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    if (writeOk) {
        writeOk = ::fsync(fd) == 0;
    }
    ::close(fd);
    if (!writeOk) {
        std::cerr << "[EB] Failed to write sessions file" << std::endl;
        ::unlink(tempPath.c_str());
        return;
    }
    if (::rename(tempPath.c_str(), filePath.c_str()) != 0) {
        std::cerr << "[EB] Failed to replace sessions file: " << std::strerror(errno) << std::endl;
        ::unlink(tempPath.c_str());
        return;
    }
    std::cout << "[EB] Sessions saved to " << filePath << std::endl;
}

void AppServer::loadSessions() {
    if (config_.dataDir.empty()) {
        return;
    }
    const std::string filePath = config_.dataDir + "/sessions.json";
    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cout << "[EB] No saved sessions at " << filePath << std::endl;
        return;
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    // Simple JSON parser for our sessions format
    std::lock_guard<std::mutex> lock(stateMutex_);
    bankSessions_.clear();

    // Find each session object
    std::size_t pos = 0;
    while (true) {
        pos = content.find("\"aspsp_name\"", pos);
        if (pos == std::string::npos) break;

        BankSession bs;

        // Extract aspsp_name
        auto extractStr = [&](const std::string& key, std::size_t searchFrom) -> std::string {
            std::size_t kp = content.find("\"" + key + "\"", searchFrom);
            if (kp == std::string::npos) return "";
            std::size_t cp = content.find(':', kp);
            if (cp == std::string::npos) return "";
            std::size_t qs = content.find('"', cp + 1);
            if (qs == std::string::npos) return "";
            std::size_t qe = content.find('"', qs + 1);
            if (qe == std::string::npos) return "";
            return content.substr(qs + 1, qe - qs - 1);
        };

        // Find the end of this session object
        std::size_t objEnd = content.find(']', pos);
        if (objEnd == std::string::npos) objEnd = content.size();
        std::size_t nextObj = content.find("\"aspsp_name\"", pos + 1);
        std::size_t searchEnd = (nextObj != std::string::npos && nextObj < objEnd) ? nextObj : objEnd;
        (void)searchEnd;

        bs.aspspName = extractStr("aspsp_name", pos);
        bs.aspspCountry = extractStr("aspsp_country", pos);
        bs.sessionId = extractStr("session_id", pos);
        bs.authorizationId = extractStr("authorization_id", pos);

        // Extract account_ids array
        std::size_t arrStart = content.find("\"account_ids\"", pos);
        if (arrStart != std::string::npos && arrStart < objEnd) {
            std::size_t bracket = content.find('[', arrStart);
            std::size_t bracketEnd = content.find(']', bracket);
            if (bracket != std::string::npos && bracketEnd != std::string::npos) {
                std::string arr = content.substr(bracket + 1, bracketEnd - bracket - 1);
                std::size_t ap = 0;
                while (true) {
                    std::size_t qs2 = arr.find('"', ap);
                    if (qs2 == std::string::npos) break;
                    std::size_t qe2 = arr.find('"', qs2 + 1);
                    if (qe2 == std::string::npos) break;
                    bs.accountIds.push_back(arr.substr(qs2 + 1, qe2 - qs2 - 1));
                    ap = qe2 + 1;
                }
            }
        }

        if (!bs.sessionId.empty()) {
            std::cout << "[EB] Loaded session: " << bs.aspspName << " (" << bs.aspspCountry
                      << ") session=" << bs.sessionId << " accounts=" << bs.accountIds.size() << std::endl;
            bankSessions_.push_back(bs);
        }

        pos += 10; // advance past current match
    }

    std::cout << "[EB] Loaded " << bankSessions_.size() << " saved sessions" << std::endl;
}

void AppServer::requestShutdown() {
    // Callable from a signal handler, so this only uses async-signal-safe
    // operations: an atomic store and shutdown(2). The sync thread is woken by the
    // destructor's notify_all, since notifying a condition_variable from a handler
    // is not safe.
    running_ = false;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
    }
}

bool AppServer::rateLimitExceeded(const std::string& bucket) {
    if (config_.rateLimitPerMinute <= 0) {
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(rateMutex_);

    // Opportunistically drop stale buckets so the map cannot grow without bound
    // when many distinct source addresses connect.
    for (auto it = rateWindows_.begin(); it != rateWindows_.end();) {
        if (now - it->second.windowStart > std::chrono::minutes(2)) {
            it = rateWindows_.erase(it);
        } else {
            ++it;
        }
    }

    RateWindow& window = rateWindows_[bucket];
    if (window.count == 0 || now - window.windowStart > std::chrono::minutes(1)) {
        window.windowStart = now;
        window.count = 1;
        return false;
    }
    ++window.count;
    return window.count > config_.rateLimitPerMinute;
}

namespace {
// Writes the whole buffer, tolerating short sends and EINTR.
bool sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t written = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}
}  // namespace

void AppServer::serve(int port) {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }
    listenFd_ = serverFd;

    int reuse = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(serverFd);
        listenFd_ = -1;
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }
    if (::listen(serverFd, 16) < 0) {
        ::close(serverFd);
        listenFd_ = -1;
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    while (running_) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        const int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        // Guard against slow/stuck clients (Slowloris) blocking the accept loop.
        timeval timeout{};
        timeout.tv_sec = 15;
        timeout.tv_usec = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        char peerText[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, peerText, sizeof(peerText));

        const std::string rawRequest = readRequest(clientFd);
        HttpRequest request = parseRequest(rawRequest);
        request.clientIp = peerText;

        HttpResponse response;
        if (request.unsupportedTransferEncoding) {
            // The parser only understands Content-Length framing; accepting a
            // chunked body would mean handling a truncated payload as complete.
            response = {411, "text/plain; charset=utf-8",
                        "chunked transfer encoding is not supported; send Content-Length", {}};
        } else if (config_.enforceHttps && !requestIsSecure(request) && !config_.allowInsecureHttp) {
            response = redirectToHttps(request);
        } else {
            response = handleRequest(request);
        }

        const std::string rawResponse = buildResponse(response);
        sendAll(clientFd, rawResponse);
        ::close(clientFd);
    }

    listenFd_ = -1;
    ::close(serverFd);
    std::cout << "HTTP listener stopped" << std::endl;
}

std::string AppServer::readRequest(int clientFd) {
    std::string request;
    char buffer[4096];
    std::size_t contentLength = 0;
    bool headersComplete = false;

    constexpr std::size_t kMaxRequestBytes = 8ULL * 1024ULL * 1024ULL; // 8 MiB total cap
    while (true) {
        const ssize_t bytesRead = ::recv(clientFd, buffer, sizeof(buffer), 0);
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(bytesRead));
        if (request.size() > kMaxRequestBytes) {
            break; // prevent OOM from oversized requests
        }
        const std::size_t headerEnd = request.find("\r\n\r\n");
        if (!headersComplete && headerEnd != std::string::npos) {
            headersComplete = true;
            const std::string headers = request.substr(0, headerEnd);
            // Case-insensitive lookup of the Content-Length header.
            const std::string lowered = lowerCopy(headers);
            const std::size_t contentLengthPos = lowered.find("content-length:");
            if (contentLengthPos != std::string::npos) {
                const std::size_t valueStart = contentLengthPos + std::strlen("content-length:");
                const std::size_t valueEnd = headers.find("\r\n", valueStart);
                const std::string rawValue = trim(headers.substr(valueStart, valueEnd - valueStart));
                // Validate the header instead of letting std::stoul throw on garbage.
                errno = 0;
                char* parseEnd = nullptr;
                const unsigned long long parsed = std::strtoull(rawValue.c_str(), &parseEnd, 10);
                constexpr unsigned long long kMaxBodyBytes = 8ULL * 1024ULL * 1024ULL;  // 8 MiB cap
                if (parseEnd != rawValue.c_str() && errno == 0 && parsed <= kMaxBodyBytes) {
                    contentLength = static_cast<std::size_t>(parsed);
                } else {
                    contentLength = 0;
                }
            }
        }
        if (headersComplete) {
            const std::size_t bodyStart = request.find("\r\n\r\n");
            const std::size_t bodySize = request.size() - (bodyStart + 4);
            if (bodySize >= contentLength) {
                break;
            }
        }
    }
    return request;
}

AppServer::HttpRequest AppServer::parseRequest(const std::string& rawRequest) {
    HttpRequest request;
    const std::size_t requestLineEnd = rawRequest.find("\r\n");
    if (requestLineEnd == std::string::npos) {
        return request;
    }
    const std::string requestLine = rawRequest.substr(0, requestLineEnd);
    std::istringstream lineStream(requestLine);
    lineStream >> request.method >> request.target;

    const std::size_t queryPos = request.target.find('?');
    request.path = queryPos == std::string::npos ? request.target : request.target.substr(0, queryPos);
    request.query = queryPos == std::string::npos ? std::string() : request.target.substr(queryPos + 1);

    const std::size_t headerStart = requestLineEnd + 2;
    const std::size_t bodyPos = rawRequest.find("\r\n\r\n");
    const std::string headerBlock = bodyPos == std::string::npos ? rawRequest.substr(headerStart) : rawRequest.substr(headerStart, bodyPos - headerStart);
    std::istringstream headersStream(headerBlock);
    std::string headerLine;
    while (std::getline(headersStream, headerLine)) {
        if (!headerLine.empty() && headerLine.back() == '\r') {
            headerLine.pop_back();
        }
        const std::size_t colonPos = headerLine.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }
        // HTTP header names are case-insensitive and HTTP/2 clients send them
        // lowercased, so normalise the key and look everything up in lower case.
        const std::string key = lowerCopy(trim(headerLine.substr(0, colonPos)));
        const std::string value = trim(headerLine.substr(colonPos + 1));
        request.headers[key] = value;
    }

    const auto transferEncoding = request.headers.find("transfer-encoding");
    if (transferEncoding != request.headers.end() &&
        lowerCopy(transferEncoding->second).find("chunked") != std::string::npos) {
        request.unsupportedTransferEncoding = true;
    }

    if (bodyPos != std::string::npos) {
        request.body = rawRequest.substr(bodyPos + 4);
    }
    return request;
}

std::string AppServer::decodeUrl(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            const char ch = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            decoded.push_back(ch);
            i += 2;
        } else if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::map<std::string, std::string> AppServer::parseQuery(const std::string& query) {
    std::map<std::string, std::string> result;
    std::size_t start = 0;
    while (start <= query.size()) {
        const std::size_t amp = query.find('&', start);
        const std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        const std::size_t eq = pair.find('=');
        const std::string key = decodeUrl(eq == std::string::npos ? pair : pair.substr(0, eq));
        const std::string value = decodeUrl(eq == std::string::npos ? std::string() : pair.substr(eq + 1));
        if (!key.empty()) {
            result[key] = value;
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return result;
}

std::string AppServer::htmlEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool AppServer::constantTimeEquals(const std::string& left, const std::string& right) {
    // Compare fixed-length SHA-256 digests rather than the raw secrets, so neither
    // the length nor the position of the first differing byte is observable in the
    // response time.
    unsigned char leftDigest[SHA256_DIGEST_LENGTH];
    unsigned char rightDigest[SHA256_DIGEST_LENGTH];
    ::SHA256(reinterpret_cast<const unsigned char*>(left.data()), left.size(), leftDigest);
    ::SHA256(reinterpret_cast<const unsigned char*>(right.data()), right.size(), rightDigest);
    return CRYPTO_memcmp(leftDigest, rightDigest, SHA256_DIGEST_LENGTH) == 0;
}

std::string AppServer::jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                // Any other C0 control character is illegal in a JSON string and
                // would produce a payload the client cannot parse. Bank-supplied
                // descriptions do occasionally contain them.
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char unicodeEscape[7];
                    std::snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(ch)));
                    escaped += unicodeEscape;
                } else {
                    escaped.push_back(ch);
                }
                break;
        }
    }
    return escaped;
}

std::string AppServer::jsonString(const std::string& value) {
    return std::string("\"") + jsonEscape(value) + "\"";
}

std::string AppServer::enumToString(my::currency currency) {
    switch (currency) {
        case my::currency::USD: return "USD";
        case my::currency::EUR: return "EUR";
        case my::currency::PLN: return "PLN";
    }
    return "EUR";
}

std::string AppServer::enumToString(my::type type) {
    switch (type) {
        case my::type::income: return "income";
        case my::type::expense: return "expense";
        case my::type::inside: return "transfer";
    }
    return "expense";
}

std::string AppServer::enumToString(my::tag tag) {
    switch (tag) {
        case my::tag::must: return "must";
        case my::tag::opt: return "opt";
        case my::tag::waste: return "waste";
    }
    return "opt";
}

bool AppServer::requestIsSecure(const HttpRequest& request) const {
    if (config_.allowInsecureHttp) {
        return true;
    }
    // Forwarding headers are attacker-controlled unless a trusted proxy rewrites
    // them, so without that guarantee treat every connection as plaintext.
    if (!config_.trustProxyHeaders) {
        return false;
    }
    const auto it = request.headers.find("x-forwarded-proto");
    if (it != request.headers.end()) {
        const std::string proto = lowerCopy(it->second);
        if (proto.find("https") != std::string::npos) {
            return true;
        }
    }
    const auto forwarded = request.headers.find("forwarded");
    if (forwarded != request.headers.end()) {
        const std::string forwardedValue = lowerCopy(forwarded->second);
        if (forwardedValue.find("proto=https") != std::string::npos) {
            return true;
        }
    }
    const auto ssl = request.headers.find("x-forwarded-ssl");
    if (ssl != request.headers.end() && lowerCopy(ssl->second) == "on") {
        return true;
    }
    return false;
}

std::string AppServer::configuredPublicBaseUrl() const {
    const std::string& uri = config_.redirectUri;
    const std::size_t schemePos = uri.find("://");
    if (schemePos == std::string::npos) {
        return std::string();
    }
    const std::size_t pathPos = uri.find('/', schemePos + 3);
    if (pathPos == std::string::npos) {
        return uri;
    }
    return uri.substr(0, pathPos);
}

std::string AppServer::buildHttpsUrl(const HttpRequest& request) const {
    // The redirect target is derived from configuration, never from the request.
    // Host / X-Forwarded-Host are attacker-controlled, and reflecting them here
    // would turn the HTTP->HTTPS upgrade into an open redirect on the very path
    // that carries OAuth codes.
    std::string host;
    const std::string base = configuredPublicBaseUrl();
    if (!base.empty()) {
        const std::size_t schemePos = base.find("://");
        if (schemePos != std::string::npos) {
            host = base.substr(schemePos + 3);
        }
    }
    if (host.empty()) {
        return config_.redirectUri;
    }

    std::ostringstream out;
    out << "https://" << host << request.target;
    return out.str();
}

std::string AppServer::generateStateToken() const {
    std::random_device device;
    std::ostringstream out;
    out << std::hex;
    for (int i = 0; i < 16; ++i) {
        const unsigned int value = device();
        out << std::setw(2) << std::setfill('0') << (value & 0xFFu);
    }
    out << std::dec;
    return out.str();
}

bool AppServer::apiAuthorized(const HttpRequest& request) const {
    if (config_.apiToken.empty()) {
        return false;
    }
    const auto auth = request.headers.find("authorization");
    if (auth == request.headers.end()) {
        return false;
    }
    const std::string prefix = "Bearer ";
    if (auth->second.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::string token = auth->second.substr(prefix.size());
    return constantTimeEquals(token, config_.apiToken);
}

AppServer::HttpResponse AppServer::redirectToHttps(const HttpRequest& request) const {
    HttpResponse response;
    response.status = 308;
    response.contentType = "text/plain; charset=utf-8";
    response.body = "redirecting to https";
    response.headers.push_back({"Location", buildHttpsUrl(request)});
    return response;
}

AppServer::HttpResponse AppServer::unauthorized(const std::string& message) const {
    return {401, "text/plain; charset=utf-8", message, {{"WWW-Authenticate", "Bearer"}}};
}

AppServer::HttpResponse AppServer::jsonResponse(int status, const std::string& body) const {
    return {status, "application/json; charset=utf-8", body, {}};
}

AppServer::HttpResponse AppServer::handleRequest(const HttpRequest& request) {
    if (config_.debugMode) {
        std::cout << "[DEBUG] AppServer handling request: " << request.method << " " << request.path << "\n";
        std::cout << "[DEBUG] Query: " << request.query << "\n";
    }

    const auto query = parseQuery(request.query);

    if (request.method == "GET" && request.path == "/health") {
        return {200, "text/plain; charset=utf-8", "ok", {}};
    }

    // Throttle only *failed* attempts against token-protected routes, so a client
    // holding a valid token is never rate limited but brute-forcing is bounded.
    const bool protectedRoute = request.path.rfind("/api/", 0) == 0 || request.path == "/sync" ||
                                request.path == "/status" || request.path == "/start-auth" ||
                                request.path == "/auth/url";
    if (protectedRoute && !apiAuthorized(request) && rateLimitExceeded("auth:" + request.clientIp)) {
        return {429, "text/plain; charset=utf-8", "too many requests", {}};
    }
    if (request.method == "GET" && request.path == "/auth/url") {
        // Bank linking spends upstream quota and mutates persisted session state,
        // so it must not be reachable without the API token.
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        try {
            const std::string aspsp = query.count("aspsp") ? query.at("aspsp") : config_.aspspName;
            const std::string country = query.count("country") ? query.at("country") : config_.aspspCountry;
            const std::string state = generateStateToken();
            const StartAuthResult authResult = client_.startAuthorization(state, aspsp, country);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                expectedAuthState_ = state;
                pendingAuthAspsp_ = aspsp;
                pendingAuthCountry_ = country;
            }
            return {200, "text/plain; charset=utf-8", authResult.url, {}};
        } catch (const std::exception& error) {
            // Upstream bodies can carry internal detail; log it, return a generic message.
            std::cerr << "[error] /auth/url: " << error.what() << std::endl;
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return {502, "text/plain; charset=utf-8", "Could not start bank authorization", {}};
        }
    }
    if (request.method == "GET" && request.path == "/start-auth") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        try {
            const std::string aspsp = query.count("aspsp") ? query.at("aspsp") : config_.aspspName;
            const std::string country = query.count("country") ? query.at("country") : config_.aspspCountry;
            const std::string state = generateStateToken();
            const StartAuthResult authResult = client_.startAuthorization(state, aspsp, country);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                expectedAuthState_ = state;
                pendingAuthAspsp_ = aspsp;
                pendingAuthCountry_ = country;
            }
            return {302, "text/plain; charset=utf-8", "redirecting", {{"Location", authResult.url}}};
        } catch (const std::exception& error) {
            std::cerr << "[error] /start-auth: " << error.what() << std::endl;
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return {502, "text/plain; charset=utf-8", "Could not start bank authorization", {}};
        }
    }
    if (request.method == "GET" && request.path == "/oauth/callback") {
        // Unauthenticated and it validates a secret, so cap the attempt rate.
        if (rateLimitExceeded("callback:" + request.clientIp)) {
            return {429, "text/plain; charset=utf-8", "too many requests", {}};
        }
        if (query.count("error") != 0) {
            const std::string errDesc = query.count("error_description") != 0 ? query.at("error_description") : "";
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                lastError_ = query.at("error") + (errDesc.empty() ? "" : ": " + errDesc);
            }
            // The provider's error text is not echoed back: it is attacker-controllable
            // for anyone who can craft a callback URL.
            return {400, "text/plain; charset=utf-8", "Bank authorization was not completed.", {}};
        }
        const std::string code = query.count("code") != 0 ? query.at("code") : std::string();
        const std::string state = query.count("state") != 0 ? query.at("state") : std::string();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (expectedAuthState_.empty() || !constantTimeEquals(expectedAuthState_, state)) {
                lastError_ = "oauth state mismatch";
                return {400, "text/plain; charset=utf-8", "Invalid OAuth state", {}};
            }
            lastAuthCode_ = code;
            expectedAuthState_.clear();
        }

        // Step 2: Exchange the code for a session via POST /sessions
        if (!code.empty()) {
            try {
                const SessionResult session = client_.createSession(code);
                std::string connectedBank;
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    BankSession bs;
                    bs.aspspName = pendingAuthAspsp_;
                    bs.aspspCountry = pendingAuthCountry_;
                    bs.sessionId = session.sessionId;
                    bs.accountIds = session.accountIds;
                    connectedBank = bs.aspspName;
                    // Replace existing session for same bank, or add new
                    bool replaced = false;
                    for (auto& existing : bankSessions_) {
                        if (existing.aspspName == bs.aspspName && existing.aspspCountry == bs.aspspCountry) {
                            existing = bs;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) {
                        bankSessions_.push_back(bs);
                    }
                    pendingAuthAspsp_.clear();
                    pendingAuthCountry_.clear();
                    lastError_.clear();
                }
                // Persist sessions to disk
                saveSessions();
                // Kick off the first sync in the background so the PSU's browser
                // is not held open for the duration of the account fetch.
                requestSync("auth-callback");
                return {200, "text/html; charset=utf-8",
                        "<html><body><h2>Authorization successful!</h2>"
                        "<p>Bank: " + htmlEscape(connectedBank) + "</p>"
                        "<p>Session established. Accounts: " + std::to_string(session.accountIds.size()) + "</p>"
                        "<p><a href=\"/\">Go to dashboard</a></p>"
                        "</body></html>", {}};
            } catch (const std::exception& error) {
                std::cerr << "[error] POST /sessions: " << error.what() << std::endl;
                std::lock_guard<std::mutex> lock(stateMutex_);
                lastError_ = std::string("POST /sessions failed: ") + error.what();
                return {502, "text/plain; charset=utf-8",
                        "Could not complete bank authorization. Please try linking the bank again.", {}};
            }
        }
        return {200, "text/plain; charset=utf-8", "Authorization received (no code). You can close this tab.", {}};
    }
    if (request.method == "POST" && request.path == "/webhook") {
        if (rateLimitExceeded("webhook:" + request.clientIp)) {
            return {429, "text/plain; charset=utf-8", "too many requests", {}};
        }
        if (!webhookSecretValid(request)) {
            return {401, "text/plain; charset=utf-8", "invalid webhook secret", {}};
        }
        if (!request.headers.count("content-type") || lowerCopy(request.headers.at("content-type")).find("application/json") == std::string::npos) {
            return {415, "text/plain; charset=utf-8", "webhook requires application/json", {}};
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastWebhookPayload_ = request.body;
        }
        // The provider expects a prompt acknowledgement, so the sync runs on the
        // background thread rather than inside this request.
        requestSync("webhook");
        return {202, "application/json; charset=utf-8", "{\"status\":\"queued\"}", {}};
    }
    if (request.method == "GET" && request.path == "/sync") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        requestSync("manual");
        return jsonResponse(202, "{\"status\":\"queued\"}");
    }
    if (request.method == "GET" && request.path == "/status") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        return {200, "text/plain; charset=utf-8", renderStatus(), {}};
    }
    if (request.method == "GET" && request.path == "/") {
        return {200, "text/html; charset=utf-8", renderHome(), {}};
    }

    if (request.method == "GET" && request.path == "/api/status") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        std::ostringstream out;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            out << "{\"status\":";
            out << jsonString("ok");
            out << ",\"accounts\":" << accounts_.size();
            out << ",\"transactions\":" << transactions_.size();
            out << ",\"lastSync\":" << jsonString(lastSyncSummary_);
            out << ",\"lastError\":" << jsonString(lastError_);
            out << "}";
        }
        return jsonResponse(200, out.str());
    }
    if (request.method == "GET" && request.path == "/api/sync-status") {
        if (!apiAuthorized(request)) return unauthorized("bad token");
        return jsonResponse(200, renderSyncStatusJson());
    }

    if (request.method == "GET" && request.path == "/api/accounts") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        return jsonResponse(200, renderAccountsJson());
    }
    if (request.method == "GET" && request.path == "/api/transactions") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        return jsonResponse(200, renderTransactionsJson());
    }
    if (request.method == "GET" && request.path == "/api/balances") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        return jsonResponse(200, renderAccountsJson());
    }

    // ===== DATABASE-BACKED API =====
    // Helper: extract path param like /api/transactions/UUID
    auto pathParam = [&](const std::string& prefix) -> std::string {
        if (request.path.rfind(prefix, 0) == 0 && request.path.size() > prefix.size()) {
            std::string rest = request.path.substr(prefix.size());
            auto slash = rest.find('/');
            return slash == std::string::npos ? rest : rest.substr(0, slash);
        }
        return "";
    };
    auto pathSuffix = [&](const std::string& prefix, const std::string& id) -> std::string {
        std::string full = prefix + id + "/";
        if (request.path.rfind(full, 0) == 0) return request.path.substr(full.size());
        return "";
    };
    // Helper: extract JSON string field from body
    auto jf = [&](const std::string& key) -> std::string {
        std::string pat = "\"" + key + "\"";
        auto p = request.body.find(pat);
        if (p == std::string::npos) return "";
        auto c = request.body.find(':', p + pat.size());
        if (c == std::string::npos) return "";
        auto vs = request.body.find_first_not_of(" \t\r\n", c + 1);
        if (vs == std::string::npos) return "";
        if (request.body[vs] == '"') {
            auto ve = request.body.find('"', vs + 1);
            return ve == std::string::npos ? "" : request.body.substr(vs + 1, ve - vs - 1);
        }
        auto ve = request.body.find_first_of(",}\r\n", vs);
        std::string raw = request.body.substr(vs, ve == std::string::npos ? std::string::npos : ve - vs);
        while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
        return raw;
    };
    auto ji = [&](const std::string& key) -> int64_t {
        std::string v = jf(key); return v.empty() ? 0 : std::strtoll(v.c_str(), nullptr, 10);
    };
    // True when the key appears in the body, even if the value is "" or 0 — needed
    // so an edit can clear a description or set an amount to zero.
    auto hasKey = [&](const std::string& key) -> bool {
        return request.body.find("\"" + key + "\"") != std::string::npos;
    };

    // --- DB Accounts ---
    if (request.path == "/api/db/accounts") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "GET") {
            auto accs = db_->accounts();
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < accs.size(); ++i) {
                if (i) o << ",";
                auto& a = accs[i];
                o << "{\"id\":" << jsonString(a.id) << ",\"name\":" << jsonString(a.name)
                  << ",\"type\":" << jsonString(a.type) << ",\"currency\":" << jsonString(a.currency)
                  << ",\"bank_name\":" << jsonString(a.bankName) << ",\"iban\":" << jsonString(a.iban)
                  << ",\"color\":" << jsonString(a.color) << ",\"balance\":" << a.balance
                  << ",\"source\":" << jsonString(a.source.empty() ? "manual" : a.source)
                  << ",\"created_at\":" << jsonString(a.createdAt)
                  << ",\"updated_at\":" << jsonString(a.updatedAt) << "}";
            }
            o << "]";
            return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Account a; a.id = db_->uuid(); a.name = jf("name"); a.type = jf("type");
            if (a.type.empty()) a.type = "wallet";
            a.currency = jf("currency"); if (a.currency.empty()) a.currency = "PLN";
            a.balance = ji("balance"); a.bankName = jf("bank_name"); a.iban = jf("iban");
            a.color = jf("color"); a.source = "manual";
            db_->upsertAccount(a);
            return jsonResponse(201, "{\"id\":" + jsonString(a.id) + "}");
        }
    }
    {
        std::string aid = pathParam("/api/db/accounts/");
        if (!aid.empty()) {
            if (!apiAuthorized(request)) return unauthorized("Unauthorized");
            if (request.method == "PUT") {
                db::Account existing = db_->account(aid);
                db::Account a; a.id = aid; a.name = jf("name"); a.type = jf("type");
                a.currency = jf("currency"); a.balance = ji("balance");
                a.bankName = jf("bank_name"); a.iban = jf("iban"); a.color = jf("color");
                a.source = existing.source.empty() ? "manual" : existing.source;
                db_->upsertAccount(a);
                return jsonResponse(200, "{\"ok\":true}");
            }
            if (request.method == "DELETE") {
                db_->deleteAccount(aid);
                return jsonResponse(200, "{\"ok\":true}");
            }
        }
    }

    // --- DB Transactions ---
    if (request.path == "/api/db/transactions") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "GET") {
            const std::string acct = query.count("account_id") ? query.at("account_id") : "";
            const std::string from = query.count("from") ? query.at("from") : "";
            const std::string to = query.count("to") ? query.at("to") : "";
            const std::string category = query.count("category") ? query.at("category") : "";
            const std::string search = query.count("q") ? query.at("q") : "";
            const int limit = query.count("limit") ? std::atoi(query.at("limit").c_str()) : 100;
            const int offset = query.count("offset") ? std::atoi(query.at("offset").c_str()) : 0;

            const db::TransactionPage page =
                    db_->transactionPage(acct, from, to, category, search, limit, offset);

            std::map<std::string, std::string> bankNames;
            for (const auto& a : db_->accounts()) bankNames[a.id] = a.bankName;

            // Splits and edit counts are fetched in one query each rather than two
            // queries per row, which previously made this endpoint issue 2N+2
            // statements for N transactions.
            std::vector<std::string> ids;
            ids.reserve(page.items.size());
            for (const auto& t : page.items) ids.push_back(t.id);

            std::map<std::string, std::vector<db::Transaction>> splitsByParent;
            for (auto& sub : db_->subTxForParents(ids)) {
                splitsByParent[sub.parentId].push_back(sub);
            }
            std::map<std::string, int64_t> editCountById;
            for (const auto& entry : db_->editCounts(ids)) {
                editCountById[entry.first] = entry.second;
            }

            std::ostringstream o;
            o << "{\"total\":" << page.total << ",\"limit\":" << limit << ",\"offset\":" << offset
              << ",\"items\":[";
            for (size_t i = 0; i < page.items.size(); ++i) {
                if (i) o << ",";
                const auto& t = page.items[i];
                const auto splitIt = splitsByParent.find(t.id);
                const int64_t editCount = editCountById.count(t.id) ? editCountById[t.id] : 0;
                o << "{\"id\":" << jsonString(t.id) << ",\"account_id\":" << jsonString(t.accountId)
                  << ",\"bank_name\":" << jsonString(bankNames[t.accountId])
                  << ",\"name\":" << jsonString(t.name) << ",\"description\":" << jsonString(t.description)
                  << ",\"amount\":" << t.amount << ",\"currency\":" << jsonString(t.currency)
                  << ",\"from\":" << jsonString(t.fromParty) << ",\"to\":" << jsonString(t.toParty)
                  << ",\"type\":" << jsonString(t.type) << ",\"category\":" << jsonString(t.category)
                  << ",\"tag\":" << jsonString(t.tag) << ",\"date\":" << jsonString(t.date)
                  << ",\"source\":" << jsonString(t.source)
                  << ",\"edited\":" << (editCount > 0 ? "true" : "false")
                  << ",\"edit_count\":" << editCount
                  << ",\"splits\":[";
                if (splitIt != splitsByParent.end()) {
                    const auto& subs = splitIt->second;
                    for (size_t j = 0; j < subs.size(); ++j) {
                        if (j) o << ",";
                        o << "{\"id\":" << jsonString(subs[j].id) << ",\"name\":" << jsonString(subs[j].name)
                          << ",\"amount\":" << subs[j].amount << ",\"category\":" << jsonString(subs[j].category) << "}";
                    }
                }
                o << "]}";
            }
            o << "]}";
            return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Transaction t; t.id = db_->uuid(); t.accountId = jf("account_id");
            t.name = jf("name"); t.description = jf("description"); t.amount = ji("amount");
            t.currency = jf("currency"); if (t.currency.empty()) t.currency = "PLN";
            t.fromParty = jf("from"); t.toParty = jf("to");
            t.type = jf("type"); if (t.type.empty()) t.type = "expense";
            t.category = jf("category"); if (t.category.empty()) t.category = "other";
            t.tag = jf("tag"); if (t.tag.empty()) t.tag = "opt";
            t.date = jf("date"); if (t.date.empty()) t.date = db::Database::now().substr(0, 10);
            t.source = "manual";
            db_->insertTx(t);
            return jsonResponse(201, "{\"id\":" + jsonString(t.id) + "}");
        }
    }
    {
        std::string tid = pathParam("/api/db/transactions/");
        if (!tid.empty()) {
            if (!apiAuthorized(request)) return unauthorized("Unauthorized");
            std::string suffix = pathSuffix("/api/db/transactions/", tid);
            if (suffix == "history" && request.method == "GET") {
                auto edits = db_->txHistory(tid);
                std::ostringstream o; o << "[";
                for (size_t i = 0; i < edits.size(); ++i) {
                    if (i) o << ",";
                    o << "{\"field\":" << jsonString(edits[i].field) << ",\"old\":" << jsonString(edits[i].oldVal)
                      << ",\"new\":" << jsonString(edits[i].newVal) << ",\"at\":" << jsonString(edits[i].editedAt) << "}";
                }
                o << "]";
                return jsonResponse(200, o.str());
            }
            if (suffix == "split" && request.method == "POST") {
                db::Transaction parent = db_->transaction(tid);
                if (parent.id.empty()) {
                    return jsonResponse(404, "{\"error\":\"transaction not found\"}");
                }
                std::vector<db::Transaction> parts;
                const std::string arrayBody = jsonFindArrayBody(request.body, "parts");
                for (const std::string& obj : jsonSplitObjects(arrayBody)) {
                    db::Transaction p;
                    p.accountId = parent.accountId;
                    p.name = jsonExtractString(obj, "name");
                    p.category = jsonExtractString(obj, "category");
                    if (p.category.empty()) p.category = parent.category;
                    p.amount = jsonExtractInt64(obj, "amount");
                    p.currency = parent.currency;
                    p.date = parent.date;
                    p.type = parent.type;
                    // Parts are user-created rows, so they must be deletable and
                    // must not inherit the parent's "bank" source.
                    p.source = "manual";
                    p.tag = parent.tag;
                    parts.push_back(p);
                }
                try {
                    db_->splitTx(tid, parts);
                } catch (const std::invalid_argument& error) {
                    return jsonResponse(422, "{\"error\":" + jsonString(error.what()) + "}");
                }
                return jsonResponse(200, "{\"ok\":true,\"parts\":" + std::to_string(parts.size()) + "}");
            }
            if (request.method == "PUT") {
                db::Transaction t = db_->transaction(tid);
                if (t.id.empty()) {
                    return jsonResponse(404, "{\"error\":\"transaction not found\"}");
                }
                // Every field the client sends is applied, including empty strings
                // (so a cleared description is recorded in the edit history).
                if (hasKey("name")) t.name = jf("name");
                if (hasKey("description")) t.description = jf("description");
                if (hasKey("category")) t.category = jf("category");
                if (hasKey("tag")) t.tag = jf("tag");
                if (hasKey("from")) t.fromParty = jf("from");
                if (hasKey("to")) t.toParty = jf("to");
                if (hasKey("type")) t.type = jf("type");
                if (hasKey("date")) t.date = jf("date");
                if (hasKey("amount")) t.amount = ji("amount");
                if (hasKey("currency")) {
                    const std::string currency = jf("currency");
                    if (!currency.empty()) t.currency = currency;
                }
                db_->updateTx(tid, t);
                return jsonResponse(200, "{\"ok\":true}");
            }
            if (request.method == "DELETE") {
                switch (db_->deleteTx(tid)) {
                    case db::DeleteResult::Deleted:
                        return jsonResponse(200, "{\"ok\":true}");
                    case db::DeleteResult::NotFound:
                        return jsonResponse(404, "{\"error\":\"transaction not found\"}");
                    case db::DeleteResult::NotDeletable:
                        return jsonResponse(409,
                                "{\"error\":\"bank transactions cannot be deleted; they would return on the next sync\"}");
                }
            }
        }
    }

    // --- Categories ---
    if (request.path == "/api/db/categories") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "GET") {
            auto cats = db_->categories();
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < cats.size(); ++i) {
                if (i) o << ",";
                o << "{\"name\":" << jsonString(cats[i].name) << ",\"icon\":" << jsonString(cats[i].icon)
                  << ",\"color\":" << jsonString(cats[i].color) << "}";
            }
            o << "]"; return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Category c; c.name = jf("name"); c.icon = jf("icon"); c.color = jf("color");
            db_->upsertCategory(c);
            return jsonResponse(201, "{\"ok\":true}");
        }
    }

    // --- Budgets ---
    if (request.path == "/api/db/budgets") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        std::string ym = query.count("month") ? query.at("month") : "";
        if (request.method == "GET") {
            auto bs = db_->budgets(ym);
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < bs.size(); ++i) {
                if (i) o << ",";
                o << "{\"category\":" << jsonString(bs[i].category)
                  << ",\"planned\":" << bs[i].planned
                  << ",\"notes\":" << jsonString(bs[i].notes)
                  << ",\"year_month\":" << jsonString(bs[i].yearMonth) << "}";
            }
            o << "]"; return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Budget b; b.yearMonth = jf("year_month"); b.category = jf("category");
            b.planned = ji("planned"); b.notes = jf("notes");
            db_->upsertBudget(b);
            return jsonResponse(201, "{\"ok\":true}");
        }
    }
    if (request.path == "/api/db/budgets/copy") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "POST") {
            const std::string from = jf("from");
            const std::string to = jf("to");
            const int n = db_->copyBudgets(from, to);
            return jsonResponse(200, "{\"ok\":true,\"copied\":" + std::to_string(n) + "}");
        }
    }
    if (request.path == "/api/db/budgets/summary") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        std::string ym = query.count("month") ? query.at("month") : "";
        auto lines = db_->budgetSummary(ym);
        std::ostringstream o; o << "[";
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) o << ",";
            o << "{\"category\":" << jsonString(lines[i].category)
              << ",\"planned\":" << lines[i].planned << ",\"actual\":" << lines[i].actual
              << ",\"notes\":" << jsonString(lines[i].notes) << "}";
        }
        o << "]"; return jsonResponse(200, o.str());
    }

    // --- Todos ---
    if (request.path == "/api/db/todos") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "GET") {
            auto ts = db_->todos();
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < ts.size(); ++i) {
                if (i) o << ",";
                o << "{\"id\":" << ts[i].id << ",\"name\":" << jsonString(ts[i].name)
                  << ",\"description\":" << jsonString(ts[i].description)
                  << ",\"amount\":" << ts[i].amount << ",\"due_date\":" << jsonString(ts[i].dueDate)
                  << ",\"done\":" << (ts[i].done ? "true" : "false") << "}";
            }
            o << "]"; return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Todo t; t.name = jf("name"); t.description = jf("description");
            t.amount = ji("amount"); t.dueDate = jf("due_date");
            auto id = db_->insertTodo(t);
            return jsonResponse(201, "{\"id\":" + std::to_string(id) + "}");
        }
    }
    {
        std::string tid = pathParam("/api/db/todos/");
        if (!tid.empty()) {
            if (!apiAuthorized(request)) return unauthorized("Unauthorized");
            int64_t id = std::strtoll(tid.c_str(), nullptr, 10);
            if (request.method == "PUT") {
                db::Todo t; t.name = jf("name"); t.description = jf("description");
                t.amount = ji("amount"); t.dueDate = jf("due_date");
                t.done = jf("done") == "true" || jf("done") == "1";
                db_->updateTodo(id, t);
                return jsonResponse(200, "{\"ok\":true}");
            }
            if (request.method == "DELETE") {
                db_->deleteTodo(id);
                return jsonResponse(200, "{\"ok\":true}");
            }
        }
    }

    // --- Savings Goals ---
    if (request.path == "/api/db/savings") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        if (request.method == "GET") {
            auto gs = db_->savingsGoals();
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < gs.size(); ++i) {
                if (i) o << ",";
                auto entries = db_->savingsEntries(gs[i].id);
                o << "{\"id\":" << gs[i].id << ",\"name\":" << jsonString(gs[i].name)
                  << ",\"target\":" << gs[i].target << ",\"deadline\":" << jsonString(gs[i].deadline)
                  << ",\"entries\":[";
                for (size_t j = 0; j < entries.size(); ++j) {
                    if (j) o << ",";
                    o << "{\"year_month\":" << jsonString(entries[j].yearMonth)
                      << ",\"planned\":" << entries[j].planned << ",\"actual\":" << entries[j].actual << "}";
                }
                o << "]}";
            }
            o << "]"; return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::SavingsGoal g; g.name = jf("name"); g.target = ji("target"); g.deadline = jf("deadline");
            auto id = db_->insertGoal(g);
            return jsonResponse(201, "{\"id\":" + std::to_string(id) + "}");
        }
    }
    {
        std::string sid = pathParam("/api/db/savings/");
        if (!sid.empty()) {
            if (!apiAuthorized(request)) return unauthorized("Unauthorized");
            int64_t id = std::strtoll(sid.c_str(), nullptr, 10);
            std::string suffix = pathSuffix("/api/db/savings/", sid);
            if (suffix == "chart" && request.method == "GET") {
                // Build the monthly progress chart from the goal's saved entries.
                db::SavingsGoal goal;
                for (const auto& g : db_->savingsGoals()) {
                    if (g.id == id) { goal = g; break; }
                }
                auto entries = db_->savingsEntries(id);
                std::ostringstream o;
                o << "{\"id\":" << id << ",\"name\":" << jsonString(goal.name)
                  << ",\"target\":" << goal.target << ",\"deadline\":" << jsonString(goal.deadline)
                  << ",\"entries\":[";
                int64_t cumulativeActual = 0;
                int64_t cumulativePlanned = 0;
                for (size_t j = 0; j < entries.size(); ++j) {
                    if (j) o << ",";
                    cumulativeActual += entries[j].actual;
                    cumulativePlanned += entries[j].planned;
                    o << "{\"year_month\":" << jsonString(entries[j].yearMonth)
                      << ",\"planned\":" << entries[j].planned
                      << ",\"actual\":" << entries[j].actual
                      << ",\"cumulative\":" << cumulativeActual
                      << ",\"cumulative_planned\":" << cumulativePlanned << "}";
                }
                o << "]}";
                return jsonResponse(200, o.str());
            }
            if (suffix == "entry" && request.method == "PUT") {
                db::SavingsEntry e; e.goalId = id; e.yearMonth = jf("year_month");
                e.planned = ji("planned"); e.actual = ji("actual");
                db_->upsertEntry(e);
                return jsonResponse(200, "{\"ok\":true}");
            }
            if (request.method == "DELETE") {
                db_->deleteGoal(id);
                return jsonResponse(200, "{\"ok\":true}");
            }
        }
    }

    // --- Month income/expense totals (dashboard) ---
    if (request.path == "/api/db/stats/totals") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        const std::string from = query.count("from") ? query.at("from") : "";
        const std::string to = query.count("to") ? query.at("to") : "";
        auto rows = db_->monthTotals(from, to);
        std::ostringstream o; o << "[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) o << ",";
            o << "{\"currency\":" << jsonString(rows[i].currency)
              << ",\"income\":" << rows[i].income
              << ",\"expense\":" << rows[i].expense << "}";
        }
        o << "]";
        return jsonResponse(200, o.str());
    }

    // --- FX stub rates (home currency conversion helper) ---
    if (request.path == "/api/db/fx/rates") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        // Static indicative rates vs PLN for demo conversion — not live market data.
        return jsonResponse(200,
            "{\"base\":\"PLN\",\"rates\":{\"PLN\":1,\"EUR\":4.3,\"USD\":3.9,\"GBP\":5.2},"
            "\"note\":\"Indicative stub rates for UI conversion only\"}");
    }

    // --- Sync History ---
    if (request.path == "/api/db/sync/history") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        auto recs = db_->syncHistory();
        std::ostringstream o; o << "[";
        for (size_t i = 0; i < recs.size(); ++i) {
            if (i) o << ",";
            o << "{\"id\":" << recs[i].id << ",\"synced_at\":" << jsonString(recs[i].syncedAt)
              << ",\"bank_name\":" << jsonString(recs[i].bankName)
              << ",\"new_tx_count\":" << recs[i].newTx << ",\"details\":" << jsonString(recs[i].details) << "}";
        }
        o << "]"; return jsonResponse(200, o.str());
    }
    if (request.path == "/api/db/sync/now" && request.method == "POST") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        requestSync("api-trigger");
        return jsonResponse(202, "{\"status\":\"queued\"}");
    }

    // --- Stats ---
    if (request.path == "/api/db/stats") {
        if (!apiAuthorized(request)) return unauthorized("Unauthorized");
        std::string from = query.count("from") ? query.at("from") : "";
        std::string to = query.count("to") ? query.at("to") : "";
        auto rows = db_->stats(from, to);
        std::ostringstream o; o << "[";
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i) o << ",";
            o << "{\"category\":" << jsonString(rows[i].category)
              << ",\"year_month\":" << jsonString(rows[i].yearMonth)
              << ",\"currency\":" << jsonString(rows[i].currency)
              << ",\"total\":" << rows[i].total << "}";
        }
        o << "]"; return jsonResponse(200, o.str());
    }

    return {404, "text/plain; charset=utf-8", "not found", {}};
}

std::string AppServer::renderStatus() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "redirect_uri=" << config_.redirectUri << "\n";
    out << "connected_banks=" << bankSessions_.size() << "\n";
    for (std::size_t i = 0; i < bankSessions_.size(); ++i) {
        const auto& s = bankSessions_[i];
        // Session IDs are bearer-equivalent credentials for the bank API, so the
        // diagnostic view reports only whether one is present.
        out << "bank[" << i << "]=" << s.aspspName << " (" << s.aspspCountry << ")"
            << " session=" << (s.sessionId.empty() ? "<none>" : "<set>")
            << " accounts=" << s.accountIds.size() << "\n";
    }
    out << "total_accounts=" << accounts_.size() << "\n";
    out << "total_transactions=" << transactions_.size() << "\n";
    out << "auth_pending=" << (!expectedAuthState_.empty() ? "yes" : "no") << "\n";
    out << "last_sync=" << (lastSyncSummary_.empty() ? "<none>" : lastSyncSummary_) << "\n";
    out << "last_error=" << (lastError_.empty() ? "<none>" : lastError_) << "\n";
    return out.str();
}

std::string AppServer::renderHome() const {
    std::ostringstream out;
    out << "<html><body>";
    out << "<h1>BanksConnectApp</h1>";
    out << "<p>This is the API host for the BanksConnect mobile app.</p>";
    // No bank-linking links and no status block: this page is reachable without a
    // token, /start-auth now requires one, and renderStatus() reports connected
    // banks and the last error. Linking is started from the mobile app, which
    // calls /auth/url with its API token and opens the returned bank URL.
    out << "<p>Link a bank from the app: Settings &rarr; Connect bank.</p>";
    out << "<p><a href=\"/health\">/health</a></p>";
    out << "</body></html>";
    return out.str();
}

std::string AppServer::renderAccountsJson() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "{\"accounts\":[";
    for (std::size_t i = 0; i < accounts_.size(); ++i) {
        const acc& account = accounts_[i];
        out << "{";
        out << "\"name\":" << jsonString(account.getName()) << ",";
        out << "\"balance\":" << account.getBalance() << ",";
        out << "\"transactions\":" << account.getTransactions().size();
        out << "}";
        if (i + 1 < accounts_.size()) {
            out << ",";
        }
    }
    out << "]}";
    return out.str();
}

std::string AppServer::renderTransactionsJson() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "{\"transactions\":[";
    for (std::size_t i = 0; i < transactions_.size(); ++i) {
        const trans& transaction = transactions_[i];
        out << "{";
        out << "\"name\":" << jsonString(transaction.name) << ",";
        out << "\"description\":" << jsonString(transaction.opis) << ",";
        out << "\"amount\":" << transaction.amount << ",";
        out << "\"currency\":" << jsonString(enumToString(transaction.curr)) << ",";
        out << "\"from\":" << jsonString(transaction.from) << ",";
        out << "\"to\":" << jsonString(transaction.to) << ",";
        out << "\"type\":" << jsonString(enumToString(transaction.type)) << ",";
        out << "\"category\":" << jsonString("other") << ",";
        out << "\"date\":" << jsonString(transaction.date) << ",";
        out << "\"tag\":" << jsonString(enumToString(transaction.tag)) << ",";
        out << "\"subtransactions\":" << transaction.subtransactions.size();
        out << "}";
        if (i + 1 < transactions_.size()) {
            out << ",";
        }
    }
    out << "]}";
    return out.str();
}

std::string AppServer::renderSyncStatusJson() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "{";
    out << "\"last_sync_time\":" << jsonString(lastSyncTime_) << ",";
    out << "\"last_sync_summary\":" << jsonString(lastSyncSummary_) << ",";
    out << "\"last_error\":" << jsonString(lastError_) << ",";
    out << "\"in_progress\":" << (syncInProgress_ ? "true" : "false") << ",";
    out << "\"sync_interval_seconds\":" << config_.syncIntervalSeconds;
    out << "}";
    return out.str();
}

std::string AppServer::buildResponse(const HttpResponse& response) const {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " ";
    switch (response.status) {
        case 200: out << "OK"; break;
        case 202: out << "Accepted"; break;
        case 301: out << "Moved Permanently"; break;
        case 302: out << "Found"; break;
        case 308: out << "Permanent Redirect"; break;
        case 400: out << "Bad Request"; break;
        case 401: out << "Unauthorized"; break;
        case 403: out << "Forbidden"; break;
        case 404: out << "Not Found"; break;
        case 409: out << "Conflict"; break;
        case 411: out << "Length Required"; break;
        case 415: out << "Unsupported Media Type"; break;
        case 422: out << "Unprocessable Entity"; break;
        case 429: out << "Too Many Requests"; break;
        case 500: out << "Internal Server Error"; break;
        case 502: out << "Bad Gateway"; break;
        case 503: out << "Service Unavailable"; break;
        default: out << "OK"; break;
    }
    out << "\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "Cache-Control: no-store\r\n";
    // Only meaningful over TLS, and only safe to advertise when HTTPS is enforced.
    if (config_.addHsts && config_.enforceHttps && !config_.allowInsecureHttp) {
        out << "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n";
    }
    out << "X-Content-Type-Options: nosniff\r\n";
    out << "Referrer-Policy: no-referrer\r\n";
    for (const auto& header : response.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n";
    out << response.body;
    return out.str();
}

bool AppServer::webhookSecretValid(const HttpRequest& request) const {
    if (config_.webhookSecret.empty()) {
        return false;
    }
    const std::string headerName = lowerCopy(
        config_.webhookSecretHeader.empty() ? "X-Webhook-Secret" : config_.webhookSecretHeader);
    const auto it = request.headers.find(headerName);
    return it != request.headers.end() && constantTimeEquals(it->second, config_.webhookSecret);
}
