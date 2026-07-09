#include "app_server.h"

#include "json_mapper.h"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
        throw std::runtime_error("ENABLEBANKING_REDIRECT_URI is required, or set RAILWAY_PUBLIC_DOMAIN/RAILWAY_STATIC_URL");
    }
    if (config_.accessToken.empty() && (config_.privateKeyPath.empty() || config_.appCode.empty())) {
        throw std::runtime_error("Set ENABLEBANKING_ACCESS_TOKEN or ENABLEBANKING_PRIVATE_KEY_PATH and ENABLEBANKING_APP_CODE");
    }

    std::cout << "Starting Railway service on port " << port << std::endl;
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
    const ::HttpResponse accountsResponse = client_.getAccounts();
    const ::HttpResponse balancesResponse = client_.getBalances();
    const ::HttpResponse transactionsResponse = client_.getTransactions();

    std::vector<acc> accounts = parseAccounts(accountsResponse.body);
    std::vector<trans> transactions = parseTransactions(transactionsResponse.body);

    if (!accounts.empty()) {
        for (const trans& transaction : transactions) {
            accounts.front().addTransaction(transaction);
        }
    }

    std::ostringstream summary;
    summary << "reason=" << reason
            << " accounts_status=" << accountsResponse.statusCode
            << " balances_status=" << balancesResponse.statusCode
            << " transactions_status=" << transactionsResponse.statusCode
            << " accounts=" << accounts.size()
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
        const HttpResponse response = handleRequest(request);
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

bool AppServer::webhookSecretValid(const HttpRequest& request) const {
    if (config_.webhookSecret.empty()) {
        return true;
    }
    const std::string headerName = config_.webhookSecretHeader.empty() ? "X-Webhook-Secret" : config_.webhookSecretHeader;
    const auto it = request.headers.find(headerName);
    return it != request.headers.end() && it->second == config_.webhookSecret;
}

AppServer::HttpResponse AppServer::handleRequest(const HttpRequest& request) {
    const auto query = parseQuery(request.query);
    if (request.method == "GET" && request.path == "/health") {
        return {200, "text/plain; charset=utf-8", "ok", {}};
    }
    if (request.method == "GET" && request.path == "/auth/url") {
        return {200, "text/plain; charset=utf-8", client_.authorizationUrl("railway-state", "accounts transactions balances"), {}};
    }
    if (request.method == "GET" && request.path == "/start-auth") {
        return {302, "text/plain; charset=utf-8", "redirecting", {{"Location", client_.authorizationUrl("railway-state", "accounts transactions balances")}}};
    }
    if (request.method == "GET" && request.path == "/oauth/callback") {
        if (query.count("error") != 0) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = query.at("error");
            return {400, "text/plain; charset=utf-8", "OAuth error: " + query.at("error"), {}};
        }
        const std::string code = query.count("code") != 0 ? query.at("code") : std::string();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastAuthCode_ = code;
            lastError_.clear();
        }
        return {200, "text/plain; charset=utf-8", "Authorization received. You can close this tab.", {}};
    }
    if (request.method == "POST" && request.path == "/webhook") {
        if (!webhookSecretValid(request)) {
            return {401, "text/plain; charset=utf-8", "invalid webhook secret", {}};
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
        try {
            syncOnce("manual");
            return {200, "text/plain; charset=utf-8", renderStatus(), {}};
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            lastError_ = error.what();
            return {500, "text/plain; charset=utf-8", error.what(), {}};
        }
    }
    if (request.method == "GET" && request.path == "/status") {
        return {200, "text/plain; charset=utf-8", renderStatus(), {}};
    }
    if (request.method == "GET" && request.path == "/") {
        return {200, "text/html; charset=utf-8", renderHome(), {}};
    }
    return {404, "text/plain; charset=utf-8", "not found", {}};
}

std::string AppServer::renderStatus() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream out;
    out << "redirect_uri=" << config_.redirectUri << "\n";
    out << "accounts=" << accounts_.size() << "\n";
    out << "transactions=" << transactions_.size() << "\n";
    out << "last_auth_code=" << (lastAuthCode_.empty() ? "<none>" : lastAuthCode_) << "\n";
    out << "last_sync=" << (lastSyncSummary_.empty() ? "<none>" : lastSyncSummary_) << "\n";
    out << "last_webhook=" << (lastWebhookPayload_.empty() ? "<none>" : lastWebhookPayload_) << "\n";
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

std::string AppServer::buildResponse(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << " ";
    switch (response.status) {
        case 200: out << "OK"; break;
        case 302: out << "Found"; break;
        case 400: out << "Bad Request"; break;
        case 401: out << "Unauthorized"; break;
        case 404: out << "Not Found"; break;
        case 500: out << "Internal Server Error"; break;
        default: out << "OK"; break;
    }
    out << "\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    for (const auto& header : response.headers) {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n";
    out << response.body;
    return out.str();
}
