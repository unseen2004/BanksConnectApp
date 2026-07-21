#include "app_server.h"
#include <locale>

#include "json_mapper.h"

#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
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
    if (syncThread_.joinable()) {
        syncThread_.join();
    }
}

void AppServer::run() {
    const char* portEnv = std::getenv("PORT");
    const int port = portEnv != nullptr && *portEnv != '\0' ? std::stoi(portEnv) : 8080;
    if (config_.redirectUri.empty()) {
        throw std::runtime_error("ENABLEBANKING_REDIRECT_URI or PUBLIC_BASE_URL is required");
    }
    if (config_.apiToken.empty()) {
        std::cout << "Warning: ENABLEBANKING_API_TOKEN is not set; /api/* endpoints will be disabled." << std::endl;
    }
    if (config_.accessToken.empty() && (config_.privateKeyPath.empty() || config_.appCode.empty())) {
        throw std::runtime_error("Set ENABLEBANKING_ACCESS_TOKEN or ENABLEBANKING_PRIVATE_KEY_PATH and ENABLEBANKING_APP_CODE");
    }
    if (config_.aspspName.empty() || config_.aspspCountry.empty()) {
        std::cout << "Warning: ENABLEBANKING_ASPSP_NAME / ENABLEBANKING_ASPSP_COUNTRY not set; /start-auth will fail." << std::endl;
    }

    std::cout << "Starting service on port " << port << std::endl;
    std::cout << "Callback URL: " << config_.redirectUri << std::endl;

    // Initialize SQLite database
    if (!config_.dataDir.empty()) {
        const std::string dbPath = config_.dataDir + "/banksconnect.db";
        db_ = std::make_unique<db::Database>(dbPath);
        std::cout << "Database: " << dbPath << std::endl;
    } else {
        db_ = std::make_unique<db::Database>(":memory:");
        std::cout << "Database: in-memory (set ENABLEBANKING_DATA_DIR for persistence)" << std::endl;
    }

    // Load persisted sessions from disk
    loadSessions();

    running_ = true;
    startSyncLoop();
    serve(port);
}

void AppServer::startSyncLoop() {
    if (config_.syncIntervalSeconds <= 0) {
        return;
    }
    syncThread_ = std::thread([this]() {
        while (running_) {
            try {
                syncOnce("poll");
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                lastError_ = error.what();
            }
            for (int i = 0; i < config_.syncIntervalSeconds && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });
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

    auto lastFour = [](const std::string& iban) -> std::string {
        if (iban.size() <= 4) return iban;
        return iban.substr(iban.size() - 4);
    };

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

            const BankAccountDetails details = parseAccountDetails(detailsResp.body);
            const BankBalance balance = parseBalance(balResp.body);

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

            db::Account dba;
            dba.id = dbAccountId;
            dba.name = friendlyName;
            dba.type = "bank";
            dba.currency = currency;
            dba.bankName = ref.bankName;
            dba.iban = details.iban;
            dba.balance = balance.minorUnits;
            dbAccounts.push_back(dba);

            // Keep an in-memory representation for /status.
            accounts.emplace_back(friendlyName, balance.minorUnits);

            const std::vector<trans> acctTx = parseTransactions(txResp.body);
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
                dbt.category = "other";
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

        accounts = parseAccounts(accountsResponse.body);
        transactions = parseTransactions(transactionsResponse.body);
        BankBalance genericBalance = parseBalance(balancesResponse.body);

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
            dbt.category = "other";
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
        for (auto& dbt : dbTransactions) {
            if (db_->txExists(dbt.bankTxId)) {
                continue;
            }
            dbt.id = db_->uuid();
            db_->insertTx(dbt);
            ++newTxCount;
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
    lastError_.clear();
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

    std::ofstream file(filePath);
    if (file.is_open()) {
        file << out.str();
        file.close();
        std::cout << "[EB] Sessions saved to " << filePath << std::endl;
    } else {
        std::cerr << "[EB] Failed to save sessions to " << filePath << std::endl;
    }
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

void AppServer::serve(int port) {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(serverFd);
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }
    if (::listen(serverFd, 16) < 0) {
        ::close(serverFd);
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    while (running_) {
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
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

        const std::string rawRequest = readRequest(clientFd);
        const HttpRequest request = parseRequest(rawRequest);
        HttpResponse response;
        if (config_.enforceHttps && !requestIsSecure(request) && !config_.allowInsecureHttp) {
            response = redirectToHttps(request);
        } else {
            response = handleRequest(request);
        }
        const std::string rawResponse = buildResponse(response);
        ::send(clientFd, rawResponse.data(), rawResponse.size(), 0);
        ::close(clientFd);
    }

    ::close(serverFd);
}

std::string AppServer::readRequest(int clientFd) {
    std::string request;
    char buffer[4096];
    std::size_t contentLength = 0;
    bool headersComplete = false;

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
        const std::string key = trim(headerLine.substr(0, colonPos));
        const std::string value = trim(headerLine.substr(colonPos + 1));
        request.headers[key] = value;
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
    if (left.size() != right.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
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
            default: escaped.push_back(ch); break;
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
    const auto it = request.headers.find("X-Forwarded-Proto");
    if (it != request.headers.end()) {
        const std::string proto = lowerCopy(it->second);
        if (proto.find("https") != std::string::npos) {
            return true;
        }
    }
    const auto forwarded = request.headers.find("Forwarded");
    if (forwarded != request.headers.end()) {
        const std::string forwardedValue = lowerCopy(forwarded->second);
        if (forwardedValue.find("proto=https") != std::string::npos) {
            return true;
        }
    }
    const auto ssl = request.headers.find("X-Forwarded-Ssl");
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
    std::string host;
    const auto forwardedHost = request.headers.find("X-Forwarded-Host");
    if (forwardedHost != request.headers.end() && !forwardedHost->second.empty()) {
        host = forwardedHost->second;
    } else {
        const auto hostHeader = request.headers.find("Host");
        if (hostHeader != request.headers.end() && !hostHeader->second.empty()) {
            host = hostHeader->second;
        }
    }
    if (host.empty()) {
        const std::string base = configuredPublicBaseUrl();
        if (!base.empty()) {
            host = base.substr(base.find("://") + 3);
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
    const auto auth = request.headers.find("Authorization");
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
    const auto query = parseQuery(request.query);

    if (request.method == "GET" && request.path == "/health") {
        return {200, "text/plain; charset=utf-8", "ok", {}};
    }
    if (request.method == "GET" && request.path == "/auth/url") {
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
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return {500, "text/plain; charset=utf-8", error.what(), {}};
        }
    }
    if (request.method == "GET" && request.path == "/start-auth") {
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
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return {500, "text/plain; charset=utf-8", error.what(), {}};
        }
    }
    if (request.method == "GET" && request.path == "/oauth/callback") {
        if (query.count("error") != 0) {
            const std::string errDesc = query.count("error_description") != 0 ? query.at("error_description") : "";
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = query.at("error") + (errDesc.empty() ? "" : ": " + errDesc);
            return {400, "text/plain; charset=utf-8", "OAuth error: " + lastError_, {}};
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
                // Immediately sync data now that we have a session
                try {
                    syncOnce("auth-callback");
                } catch (const std::exception& syncErr) {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    lastError_ = std::string("sync after auth: ") + syncErr.what();
                }
                return {200, "text/html; charset=utf-8",
                        "<html><body><h2>Authorization successful!</h2>"
                        "<p>Bank: " + htmlEscape(connectedBank) + "</p>"
                        "<p>Session established. Accounts: " + std::to_string(session.accountIds.size()) + "</p>"
                        "<p><a href=\"/\">Go to dashboard</a></p>"
                        "</body></html>", {}};
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                lastError_ = std::string("POST /sessions failed: ") + error.what();
                return {500, "text/plain; charset=utf-8", "Session creation failed: " + std::string(error.what()), {}};
            }
        }
        return {200, "text/plain; charset=utf-8", "Authorization received (no code). You can close this tab.", {}};
    }
    if (request.method == "POST" && request.path == "/webhook") {
        if (!webhookSecretValid(request)) {
            return {401, "text/plain; charset=utf-8", "invalid webhook secret", {}};
        }
        if (!request.headers.count("Content-Type") || lowerCopy(request.headers.at("Content-Type")).find("application/json") == std::string::npos) {
            return {415, "text/plain; charset=utf-8", "webhook requires application/json", {}};
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastWebhookPayload_ = request.body;
        }
        try {
            syncOnce("webhook");
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
        }
        return {200, "text/plain; charset=utf-8", "ok", {}};
    }
    if (request.method == "GET" && request.path == "/sync") {
        if (!apiAuthorized(request)) {
            return unauthorized(config_.apiToken.empty() ? "API token not configured" : "Unauthorized");
        }
        try {
            syncOnce("manual");
            return jsonResponse(200, renderStatus());
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return jsonResponse(500, std::string("{\"error\":") + jsonString(error.what()) + "}");
        }
    }
    if (request.method == "GET" && request.path == "/status") {
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
                  << ",\"balance\":" << a.balance << ",\"created_at\":" << jsonString(a.createdAt)
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
            db_->upsertAccount(a);
            return jsonResponse(201, "{\"id\":" + jsonString(a.id) + "}");
        }
    }
    {
        std::string aid = pathParam("/api/db/accounts/");
        if (!aid.empty()) {
            if (!apiAuthorized(request)) return unauthorized("Unauthorized");
            if (request.method == "PUT") {
                db::Account a; a.id = aid; a.name = jf("name"); a.type = jf("type");
                a.currency = jf("currency"); a.balance = ji("balance");
                a.bankName = jf("bank_name"); a.iban = jf("iban");
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
            std::string acct = query.count("account_id") ? query.at("account_id") : "";
            std::string from = query.count("from") ? query.at("from") : "";
            std::string to = query.count("to") ? query.at("to") : "";
            auto txs = db_->transactions(acct, from, to);
            std::ostringstream o; o << "[";
            for (size_t i = 0; i < txs.size(); ++i) {
                if (i) o << ",";
                auto& t = txs[i];
                auto subs = db_->subTx(t.id);
                auto edits = db_->txHistory(t.id);
                o << "{\"id\":" << jsonString(t.id) << ",\"account_id\":" << jsonString(t.accountId)
                  << ",\"name\":" << jsonString(t.name) << ",\"description\":" << jsonString(t.description)
                  << ",\"amount\":" << t.amount << ",\"currency\":" << jsonString(t.currency)
                  << ",\"from\":" << jsonString(t.fromParty) << ",\"to\":" << jsonString(t.toParty)
                  << ",\"type\":" << jsonString(t.type) << ",\"category\":" << jsonString(t.category)
                  << ",\"tag\":" << jsonString(t.tag) << ",\"date\":" << jsonString(t.date)
                  << ",\"source\":" << jsonString(t.source)
                  << ",\"edited\":" << (edits.empty() ? "false" : "true")
                  << ",\"edit_count\":" << edits.size()
                  << ",\"splits\":[";
                for (size_t j = 0; j < subs.size(); ++j) {
                    if (j) o << ",";
                    o << "{\"id\":" << jsonString(subs[j].id) << ",\"name\":" << jsonString(subs[j].name)
                      << ",\"amount\":" << subs[j].amount << ",\"category\":" << jsonString(subs[j].category) << "}";
                }
                o << "]}";
            }
            o << "]";
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
                // Parse parts array from body — simplified: expects {"parts":[{"name":"A","amount":100,"category":"food"}, ...]}
                db::Transaction parent = db_->transaction(tid);
                std::vector<db::Transaction> parts;
                std::string body = request.body;
                size_t pos = 0;
                while (true) {
                    pos = body.find("{", pos + 1);
                    if (pos == std::string::npos || pos < 10) break; // skip outer {
                    size_t end = body.find("}", pos);
                    if (end == std::string::npos) break;
                    std::string obj = body.substr(pos, end - pos + 1);
                    // mini parse
                    auto of = [&](const std::string& k) -> std::string {
                        auto p2 = obj.find("\"" + k + "\"");
                        if (p2 == std::string::npos) return "";
                        auto c = obj.find(':', p2); if (c == std::string::npos) return "";
                        auto vs = obj.find_first_not_of(" \t\"", c + 1);
                        auto ve = obj.find_first_of(",}\"", vs);
                        return obj.substr(vs, ve - vs);
                    };
                    db::Transaction p; p.accountId = parent.accountId;
                    p.name = of("name"); p.category = of("category");
                    std::string amt = of("amount"); p.amount = amt.empty() ? 0 : std::strtoll(amt.c_str(), nullptr, 10);
                    p.currency = parent.currency; p.date = parent.date; p.type = parent.type;
                    p.source = parent.source; p.tag = parent.tag;
                    parts.push_back(p);
                    pos = end;
                }
                db_->splitTx(tid, parts);
                return jsonResponse(200, "{\"ok\":true,\"parts\":" + std::to_string(parts.size()) + "}");
            }
            if (request.method == "PUT") {
                db::Transaction t = db_->transaction(tid);
                if (!jf("name").empty()) t.name = jf("name");
                if (!jf("description").empty()) t.description = jf("description");
                if (!jf("category").empty()) t.category = jf("category");
                if (!jf("tag").empty()) t.tag = jf("tag");
                if (!jf("from").empty()) t.fromParty = jf("from");
                if (!jf("to").empty()) t.toParty = jf("to");
                if (!jf("type").empty()) t.type = jf("type");
                if (!jf("date").empty()) t.date = jf("date");
                if (!jf("amount").empty()) t.amount = ji("amount");
                db_->updateTx(tid, t);
                return jsonResponse(200, "{\"ok\":true}");
            }
            if (request.method == "DELETE") {
                db_->deleteTx(tid);
                return jsonResponse(200, "{\"ok\":true}");
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
                  << ",\"planned\":" << bs[i].planned << ",\"year_month\":" << jsonString(bs[i].yearMonth) << "}";
            }
            o << "]"; return jsonResponse(200, o.str());
        }
        if (request.method == "POST") {
            db::Budget b; b.yearMonth = jf("year_month"); b.category = jf("category"); b.planned = ji("planned");
            db_->upsertBudget(b);
            return jsonResponse(201, "{\"ok\":true}");
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
              << ",\"planned\":" << lines[i].planned << ",\"actual\":" << lines[i].actual << "}";
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
                int64_t cumulative = 0;
                for (size_t j = 0; j < entries.size(); ++j) {
                    if (j) o << ",";
                    cumulative += entries[j].actual;
                    o << "{\"year_month\":" << jsonString(entries[j].yearMonth)
                      << ",\"planned\":" << entries[j].planned
                      << ",\"actual\":" << entries[j].actual
                      << ",\"cumulative\":" << cumulative << "}";
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
        try { syncOnce("api-trigger"); return jsonResponse(200, "{\"ok\":true}"); }
        catch (const std::exception& e) { return jsonResponse(500, "{\"error\":" + jsonString(e.what()) + "}"); }
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
              << ",\"year_month\":" << jsonString(rows[i].yearMonth) << ",\"total\":" << rows[i].total << "}";
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
        out << "bank[" << i << "]=" << s.aspspName << " (" << s.aspspCountry << ")"
            << " session=" << s.sessionId << " accounts=" << s.accountIds.size() << "\n";
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
    out << "<h3>Connect a bank:</h3>";
    out << "<ul>";
    out << "<li><a href=\"/start-auth?aspsp=Bank%20Millennium&country=PL\">Bank Millennium</a></li>";
    out << "<li><a href=\"/start-auth?aspsp=PKO&country=PL\">PKO BP</a></li>";
    out << "<li><a href=\"/start-auth?aspsp=mBank&country=PL\">mBank</a></li>";
    out << "<li><a href=\"/start-auth?aspsp=ING&country=PL\">ING</a></li>";
    out << "<li><a href=\"/start-auth?aspsp=Santander&country=PL\">Santander</a></li>";
    out << "</ul>";
    out << "<h3>Status:</h3>";
    out << "<pre>" << htmlEscape(renderStatus()) << "</pre>";
    out << "<p><a href=\"/health\">/health</a> | <a href=\"/status\">/status</a></p>";
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
    out << "\"sync_interval_seconds\":" << config_.syncIntervalSeconds;
    out << "}";
    return out.str();
}

std::string AppServer::buildResponse(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " ";
    switch (response.status) {
        case 200: out << "OK"; break;
        case 301: out << "Moved Permanently"; break;
        case 302: out << "Found"; break;
        case 308: out << "Permanent Redirect"; break;
        case 400: out << "Bad Request"; break;
        case 401: out << "Unauthorized"; break;
        case 404: out << "Not Found"; break;
        case 415: out << "Unsupported Media Type"; break;
        case 500: out << "Internal Server Error"; break;
        default: out << "OK"; break;
    }
    out << "\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "Cache-Control: no-store\r\n";
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
    const std::string headerName = config_.webhookSecretHeader.empty() ? "X-Webhook-Secret" : config_.webhookSecretHeader;
    const auto it = request.headers.find(headerName);
    return it != request.headers.end() && constantTimeEquals(it->second, config_.webhookSecret);
}
