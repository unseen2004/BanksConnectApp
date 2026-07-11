#include "app_server.h"

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
    // Check if we have a valid Enable Banking session with account IDs
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ids = accountIds_;
    }

    std::vector<acc> accounts;
    std::vector<trans> transactions;
    std::ostringstream summary;
    summary << "reason=" << reason;

    if (!ids.empty()) {
        // Use proper per-account Enable Banking endpoints
        for (const std::string& accountId : ids) {
            const ::HttpResponse balResp = client_.getAccountBalances(accountId);
            const ::HttpResponse txResp = client_.getAccountTransactions(accountId);

            summary << " account=" << accountId
                    << " bal_status=" << balResp.statusCode
                    << " tx_status=" << txResp.statusCode;

            // Parse balance — extract amount from first balance entry
            std::string balanceStr = "0";
            {
                const std::size_t amountPos = balResp.body.find("\"amount\"");
                if (amountPos != std::string::npos) {
                    const std::size_t colonPos = balResp.body.find(':', amountPos);
                    if (colonPos != std::string::npos) {
                        std::size_t valStart = balResp.body.find_first_not_of(" \t\r\n", colonPos + 1);
                        if (valStart != std::string::npos) {
                            if (balResp.body[valStart] == '"') ++valStart;
                            std::size_t valEnd = balResp.body.find_first_of(",}\"\r\n", valStart);
                            balanceStr = balResp.body.substr(valStart, valEnd - valStart);
                        }
                    }
                }
            }
            int balance = 0;
            try { balance = static_cast<int>(std::stod(balanceStr)); } catch (...) {}

            accounts.emplace_back(accountId, balance);

            std::vector<trans> acctTx = parseTransactions(txResp.body);
            for (const trans& t : acctTx) {
                accounts.back().addTransaction(t);
                transactions.push_back(t);
            }
        }
    } else {
        // Fallback to legacy generic endpoints (before session is established)
        const ::HttpResponse accountsResponse = client_.getAccounts();
        const ::HttpResponse balancesResponse = client_.getBalances();
        const ::HttpResponse transactionsResponse = client_.getTransactions();

        accounts = parseAccounts(accountsResponse.body);
        transactions = parseTransactions(transactionsResponse.body);

        if (!accounts.empty()) {
            for (const trans& transaction : transactions) {
                accounts.front().addTransaction(transaction);
            }
        }

        summary << " accounts_status=" << accountsResponse.statusCode
                << " balances_status=" << balancesResponse.statusCode
                << " transactions_status=" << transactionsResponse.statusCode;
    }

    summary << " accounts=" << accounts.size()
            << " transactions=" << transactions.size();

    std::lock_guard<std::mutex> lock(stateMutex_);
    accounts_ = std::move(accounts);
    transactions_ = std::move(transactions);
    lastSyncSummary_ = summary.str();
    lastError_.clear();
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
            const std::size_t contentLengthPos = headers.find("Content-Length:");
            if (contentLengthPos != std::string::npos) {
                const std::size_t valueStart = contentLengthPos + std::strlen("Content-Length:");
                const std::size_t valueEnd = headers.find("\r\n", valueStart);
                contentLength = static_cast<std::size_t>(std::stoul(trim(headers.substr(valueStart, valueEnd - valueStart))));
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
        case my::type::inside: return "inside";
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
            const std::string state = generateStateToken();
            const StartAuthResult authResult = client_.startAuthorization(state);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                expectedAuthState_ = state;
                authorizationId_ = authResult.authorizationId;
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
            const std::string state = generateStateToken();
            const StartAuthResult authResult = client_.startAuthorization(state);
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                expectedAuthState_ = state;
                authorizationId_ = authResult.authorizationId;
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
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    sessionId_ = session.sessionId;
                    accountIds_ = session.accountIds;
                    lastError_.clear();
                }
                // Immediately sync data now that we have a session
                try {
                    syncOnce("auth-callback");
                } catch (const std::exception& syncErr) {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    lastError_ = std::string("sync after auth: ") + syncErr.what();
                }
                return {200, "text/html; charset=utf-8",
                        "<html><body><h2>Authorization successful!</h2>"
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

    return {404, "text/plain; charset=utf-8", "not found", {}};
}

std::string AppServer::renderStatus() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "redirect_uri=" << config_.redirectUri << "\n";
    out << "aspsp=" << config_.aspspName << " (" << config_.aspspCountry << ")\n";
    out << "session_id=" << (sessionId_.empty() ? "<none>" : sessionId_) << "\n";
    out << "eb_accounts=" << accountIds_.size() << "\n";
    out << "accounts=" << accounts_.size() << "\n";
    out << "transactions=" << transactions_.size() << "\n";
    out << "auth_pending=" << (!expectedAuthState_.empty() ? "yes" : "no") << "\n";
    out << "last_sync=" << (lastSyncSummary_.empty() ? "<none>" : lastSyncSummary_) << "\n";
    out << "last_error=" << (lastError_.empty() ? "<none>" : lastError_) << "\n";
    return out.str();
}

std::string AppServer::renderHome() const {
    std::ostringstream out;
    out << "<html><body>";
    out << "<h1>BanksConnectApp</h1>";
    out << "<ul>";
    out << "<li><a href=\"/health\">/health</a></li>";
    out << "<li><a href=\"/auth/url\">/auth/url</a></li>";
    out << "<li><a href=\"/start-auth\">/start-auth</a></li>";
    out << "<li><a href=\"/status\">/status</a></li>";
    out << "</ul>";
    out << "<pre>" << htmlEscape(renderStatus()) << "</pre>";
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
