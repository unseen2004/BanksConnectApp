#include "enablebanking_client.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <utility>
#include <vector>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

namespace {
constexpr const char* kStatusMarker = "\n__ENABLEBANKING_STATUS__:";

std::string readAll(int fd) {
    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t bytesRead = ::read(fd, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        if (bytesRead == 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(bytesRead));
    }
    return output;
}

std::string jsonEscape(const std::string& value) {
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

std::string makeJson(const EnableBankingConfig& config, long iat, long exp) {
    return std::string("{\"iss\":\"") + jsonEscape(config.jwtIssuer) + "\",\"aud\":\"" +
           jsonEscape(config.jwtAudience) + "\",\"iat\":" + std::to_string(iat) +
           ",\"exp\":" + std::to_string(exp) + "}";
}

/// Extract a string value for a given key from a flat JSON object.
/// Handles quoted string values only (not nested objects/arrays).
std::string extractJsonString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const std::size_t colonPos = json.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) {
        return std::string();
    }
    std::size_t valueStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) {
        return std::string();
    }
    if (json[valueStart] == '"') {
        const std::size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) {
            return std::string();
        }
        return json.substr(valueStart + 1, valueEnd - valueStart - 1);
    }
    // Non-string value
    const std::size_t valueEnd = json.find_first_of(",}\r\n", valueStart);
    std::string raw = json.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
    // Trim whitespace
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
        raw.pop_back();
    }
    return raw;
}

/// Extract all account UUIDs from a POST /sessions response.
/// The accounts are returned as objects with "uid" fields.
std::vector<std::string> extractAccountIds(const std::string& json) {
    std::vector<std::string> ids;
    const std::string uidKey = "\"uid\"";
    std::size_t pos = 0;
    while (true) {
        pos = json.find(uidKey, pos);
        if (pos == std::string::npos) {
            break;
        }
        const std::size_t colonPos = json.find(':', pos + uidKey.size());
        if (colonPos == std::string::npos) {
            break;
        }
        std::size_t valueStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
        if (valueStart == std::string::npos || json[valueStart] != '"') {
            pos = colonPos + 1;
            continue;
        }
        const std::size_t valueEnd = json.find('"', valueStart + 1);
        if (valueEnd == std::string::npos) {
            break;
        }
        ids.push_back(json.substr(valueStart + 1, valueEnd - valueStart - 1));
        pos = valueEnd + 1;
    }

    // Fallback: also try "account_id" at top-level accounts array
    // Some responses use account UUIDs directly in the "accounts" array as strings
    if (ids.empty()) {
        const std::string accountIdKey = "\"account_id\"";
        pos = 0;
        while (true) {
            pos = json.find(accountIdKey, pos);
            if (pos == std::string::npos) {
                break;
            }
            // Check if this is a nested object or a string
            const std::size_t colonPos = json.find(':', pos + accountIdKey.size());
            if (colonPos == std::string::npos) {
                break;
            }
            pos = colonPos + 1;
        }
    }

    return ids;
}
}

EnableBankingClient::EnableBankingClient(EnableBankingConfig config) : config_(std::move(config)) {}

std::string EnableBankingClient::urlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char ch : value) {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                          ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            encoded.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            encoded.append("%20");
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }
    return encoded;
}

std::string EnableBankingClient::base64UrlEncode(const unsigned char* data, std::size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve(((size + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= size) {
        const unsigned int chunk = (static_cast<unsigned int>(data[i]) << 16) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8) |
                                   static_cast<unsigned int>(data[i + 2]);
        output.push_back(alphabet[(chunk >> 18) & 0x3F]);
        output.push_back(alphabet[(chunk >> 12) & 0x3F]);
        output.push_back(alphabet[(chunk >> 6) & 0x3F]);
        output.push_back(alphabet[chunk & 0x3F]);
        i += 3;
    }

    const std::size_t remaining = size - i;
    if (remaining == 1) {
        const unsigned int chunk = static_cast<unsigned int>(data[i]) << 16;
        output.push_back(alphabet[(chunk >> 18) & 0x3F]);
        output.push_back(alphabet[(chunk >> 12) & 0x3F]);
    } else if (remaining == 2) {
        const unsigned int chunk = (static_cast<unsigned int>(data[i]) << 16) |
                                   (static_cast<unsigned int>(data[i + 1]) << 8);
        output.push_back(alphabet[(chunk >> 18) & 0x3F]);
        output.push_back(alphabet[(chunk >> 12) & 0x3F]);
        output.push_back(alphabet[(chunk >> 6) & 0x3F]);
    }

    return output;
}

std::string EnableBankingClient::base64UrlEncode(const std::string& value) {
    return base64UrlEncode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

std::string EnableBankingClient::signWithOpenSsl(const std::string& message, const std::string& privateKeyPath) {
    FILE* keyFile = fopen(privateKeyPath.c_str(), "r");
    if (!keyFile) throw std::runtime_error("Could not open private key file: " + privateKeyPath);
    EVP_PKEY* pkey = PEM_read_PrivateKey(keyFile, nullptr, nullptr, nullptr);
    fclose(keyFile);
    if (!pkey) throw std::runtime_error("Could not read private key from " + privateKeyPath);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(pkey); throw std::runtime_error("EVP_MD_CTX_new failed"); }

    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); throw std::runtime_error("EVP_DigestSignInit failed");
    }

    if (EVP_DigestSignUpdate(ctx, message.data(), message.size()) <= 0) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); throw std::runtime_error("EVP_DigestSignUpdate failed");
    }

    size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &sigLen) <= 0) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); throw std::runtime_error("EVP_DigestSignFinal failed (length)");
    }

    std::string signature(sigLen, '\0');
    if (EVP_DigestSignFinal(ctx, reinterpret_cast<unsigned char*>(&signature[0]), &sigLen) <= 0) {
        EVP_MD_CTX_free(ctx); EVP_PKEY_free(pkey); throw std::runtime_error("EVP_DigestSignFinal failed");
    }
    signature.resize(sigLen);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return signature;
}

std::string EnableBankingClient::jsonEscapeStatic(const std::string& value) {
    return jsonEscape(value);
}

std::string EnableBankingClient::generateJwt() const {
    if (config_.privateKeyPath.empty() || config_.appCode.empty()) {
        throw std::runtime_error("ENABLEBANKING_ACCESS_TOKEN or ENABLEBANKING_PRIVATE_KEY_PATH and ENABLEBANKING_APP_CODE are required");
    }

    const long now = static_cast<long>(std::time(nullptr));
    const long exp = now + (config_.jwtTtlSeconds > 0 ? config_.jwtTtlSeconds : 3600);
    const std::string header = std::string("{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"") + jsonEscape(config_.appCode) + "\"}";
    const std::string payload = makeJson(config_, now, exp);
    const std::string signingInput = base64UrlEncode(header) + "." + base64UrlEncode(payload);
    const std::string signature = signWithOpenSsl(signingInput, config_.privateKeyPath);
    return signingInput + "." + base64UrlEncode(signature);
}

std::string EnableBankingClient::bearerToken() const {
    if (!config_.accessToken.empty()) {
        return config_.accessToken;
    }
    return generateJwt();
}

std::string EnableBankingClient::makeUrl(const std::string& path) const {
    if (path.empty()) {
        return config_.baseUrl;
    }
    if (!path.empty() && path.front() == '/') {
        return config_.baseUrl + path;
    }
    return config_.baseUrl + "/" + path;
}

std::string EnableBankingClient::authorizationUrl(const std::string& state, const std::string& scope) const {
    // Legacy method — kept for CLI --auth-url mode.
    // For proper Enable Banking flow, use startAuthorization() instead.
    if (config_.clientId.empty() || config_.redirectUri.empty()) {
        throw std::runtime_error("ENABLEBANKING_CLIENT_ID and ENABLEBANKING_REDIRECT_URI are required");
    }
    return makeUrl(config_.authorizePath) + "?response_type=code&client_id=" + urlEncode(config_.clientId) +
           "&redirect_uri=" + urlEncode(config_.redirectUri) + "&scope=" + urlEncode(scope) +
           "&state=" + urlEncode(state);
}

// ===== Enable Banking proper flow =====

StartAuthResult EnableBankingClient::startAuthorization(const std::string& state) const {
    return startAuthorization(state, config_.aspspName, config_.aspspCountry);
}

StartAuthResult EnableBankingClient::startAuthorization(const std::string& state,
                                                         const std::string& aspspName,
                                                         const std::string& aspspCountry) const {
    if (aspspName.empty() || aspspCountry.empty()) {
        throw std::runtime_error("ASPSP name and country are required for authorization");
    }
    if (config_.redirectUri.empty()) {
        throw std::runtime_error("ENABLEBANKING_REDIRECT_URI or PUBLIC_BASE_URL is required");
    }

    // Compute valid_until: now + consentValidDays
    const std::time_t now = std::time(nullptr);
    const std::time_t validUntil = now + static_cast<std::time_t>(config_.consentValidDays) * 86400;
    struct tm utcTime{};
    gmtime_r(&validUntil, &utcTime);
    char validUntilStr[64];
    std::strftime(validUntilStr, sizeof(validUntilStr), "%Y-%m-%dT%H:%M:%SZ", &utcTime);

    const std::string psuType = config_.psuType.empty() ? "personal" : config_.psuType;

    // Build the POST /auth request body
    std::string body = "{";
    body += "\"access\":{";
    body += "\"valid_until\":\"" + jsonEscape(std::string(validUntilStr)) + "\"";
    body += ",\"balances\":true";
    body += ",\"transactions\":true";
    body += "},";
    body += "\"aspsp\":{";
    body += "\"name\":\"" + jsonEscape(aspspName) + "\"";
    body += ",\"country\":\"" + jsonEscape(aspspCountry) + "\"";
    body += "},";
    body += "\"state\":\"" + jsonEscape(state) + "\",";
    body += "\"redirect_url\":\"" + jsonEscape(config_.redirectUri) + "\",";
    body += "\"psu_type\":\"" + jsonEscape(psuType) + "\"";
    body += "}";

    std::cout << "[EB] POST /auth body: " << body << std::endl;

    const HttpResponse response = post("/auth", body);

    std::cout << "[EB] POST /auth status: " << response.statusCode
              << " body: " << response.body << std::endl;

    if (response.statusCode < 200 || response.statusCode >= 300) {
        throw std::runtime_error("POST /auth failed (HTTP " + std::to_string(response.statusCode) + "): " + response.body);
    }

    StartAuthResult result;
    result.url = extractJsonString(response.body, "url");
    result.authorizationId = extractJsonString(response.body, "authorization_id");

    if (result.url.empty()) {
        throw std::runtime_error("POST /auth response missing 'url' field: " + response.body);
    }

    return result;
}

SessionResult EnableBankingClient::createSession(const std::string& code) const {
    const std::string body = "{\"code\":\"" + jsonEscape(code) + "\"}";

    std::cout << "[EB] POST /sessions body: " << body << std::endl;

    const HttpResponse response = post("/sessions", body);

    std::cout << "[EB] POST /sessions status: " << response.statusCode
              << " body: " << response.body << std::endl;

    if (response.statusCode < 200 || response.statusCode >= 300) {
        throw std::runtime_error("POST /sessions failed (HTTP " + std::to_string(response.statusCode) + "): " + response.body);
    }

    SessionResult result;
    result.sessionId = extractJsonString(response.body, "session_id");
    result.accountIds = extractAccountIds(response.body);
    result.rawJson = response.body;

    if (result.sessionId.empty()) {
        throw std::runtime_error("POST /sessions response missing 'session_id': " + response.body);
    }

    std::cout << "[EB] Session created: id=" << result.sessionId
              << " accounts=" << result.accountIds.size() << std::endl;

    return result;
}

HttpResponse EnableBankingClient::getAccountDetails(const std::string& accountId) const {
    return get("/accounts/" + accountId + "/details");
}

HttpResponse EnableBankingClient::getAccountBalances(const std::string& accountId) const {
    return get("/accounts/" + accountId + "/balances");
}

HttpResponse EnableBankingClient::getAccountTransactions(const std::string& accountId) const {
    return get("/accounts/" + accountId + "/transactions");
}

HttpResponse EnableBankingClient::createConsent(const std::string& consentPayloadJson) const {
    return post(config_.consentPath, consentPayloadJson);
}

HttpResponse EnableBankingClient::getAccounts() const {
    return get(config_.accountsPath);
}

HttpResponse EnableBankingClient::getBalances() const {
    return get(config_.balancesPath);
}

HttpResponse EnableBankingClient::getTransactions() const {
    return get(config_.transactionsPath);
}

HttpResponse EnableBankingClient::get(const std::string& path) const {
    return request("GET", path, "");
}

HttpResponse EnableBankingClient::post(const std::string& path, const std::string& body) const {
    return request("POST", path, body);
}

HttpResponse EnableBankingClient::request(const std::string& method, const std::string& path, const std::string& body) const {
    int pipefd[2];
    if (::pipe(pipefd) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }

    const std::string url = makeUrl(path);
    const std::string authHeader = "Authorization: Bearer " + bearerToken();
    const std::string contentTypeHeader = "Content-Type: application/json";
    const std::string acceptHeader = "Accept: application/json";

    std::vector<std::string> args;
    args.emplace_back("curl");
    args.emplace_back("-sS");
    args.emplace_back("-X");
    args.emplace_back(method);
    args.emplace_back("-w");
    args.emplace_back(std::string(kStatusMarker) + "%{http_code}");
    args.emplace_back("-H");
    args.emplace_back(contentTypeHeader);
    args.emplace_back("-H");
    args.emplace_back(acceptHeader);
    args.emplace_back("-H");
    args.emplace_back(authHeader);
    if (!body.empty()) {
        args.emplace_back("--data-binary");
        args.emplace_back(body);
    }
    args.emplace_back(url);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        ::execvp("curl", argv.data());
        _exit(127);
    }

    ::close(pipefd[1]);
    const std::string output = readAll(pipefd[0]);
    ::close(pipefd[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
    }

    HttpResponse response;
    const std::size_t markerPos = output.rfind(kStatusMarker);
    if (markerPos != std::string::npos) {
        response.body = output.substr(0, markerPos);
        const std::string statusText = output.substr(markerPos + std::strlen(kStatusMarker));
        response.statusCode = std::strtol(statusText.c_str(), nullptr, 10);
    } else {
        response.body = output;
        response.statusCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return response;
}
