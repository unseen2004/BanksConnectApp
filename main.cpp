#include "app_server.h"
#include "enablebanking_client.h"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::string envOrEmpty(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? std::string() : std::string(value);
}

int envOrInt(const char* key, int fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool envOrBool(const char* key, bool fallback) {
    const std::string value = envOrEmpty(key);
    if (value.empty()) {
        return fallback;
    }
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES" || value == "on" || value == "ON") {
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" || value == "NO" || value == "off" || value == "OFF") {
        return false;
    }
    return fallback;
}

std::string stripScheme(std::string value) {
    if (value.rfind("https://", 0) == 0) {
        return value.substr(8);
    }
    if (value.rfind("http://", 0) == 0) {
        return value.substr(7);
    }
    return value;
}

std::string normalizePublicBaseUrl(std::string value) {
    if (value.empty()) {
        return value;
    }
    if (value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0) {
        value = "https://" + value;
    }
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

std::string envFirst(const char* const* keys, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::string value = envOrEmpty(keys[i]);
        if (!value.empty()) {
            return value;
        }
    }
    return std::string();
}

std::string derivePublicBaseUrl() {
    const char* keys[] = {
            "PUBLIC_BASE_URL",
            "AWS_PUBLIC_BASE_URL",
            "AWS_PUBLIC_URL",
            "AWS_DOMAIN_NAME",
            "AWS_LB_DNS_NAME",
            "AWS_ELB_DNS_NAME",
            "AWS_HOSTNAME"
    };
    return normalizePublicBaseUrl(envFirst(keys, sizeof(keys) / sizeof(keys[0])));
}

std::string base64Decode(const std::string& input) {
    auto decodeChar = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };

    std::string clean;
    for (char ch : input) {
        if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') {
            clean.push_back(ch);
        }
    }

    std::string output;
    int val = 0;
    int valb = -8;
    for (char ch : clean) {
        if (ch == '=') {
            break;
        }
        const int decoded = decodeChar(ch);
        if (decoded < 0) {
            throw std::runtime_error("Invalid base64 private key payload");
        }
        val = (val << 6) + decoded;
        valb += 6;
        if (valb >= 0) {
            output.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

// Returns the signing key as PEM text. The key is kept in memory only: it used to
// be written to a temp file in /tmp so the openssl CLI could read it, which left
// the private key on disk for the lifetime of the process and leaked it entirely
// if the process was killed before its atexit handler ran.
std::string loadPrivateKeyPem() {
    const std::string keyPath = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PATH");
    if (!keyPath.empty()) {
        std::ifstream keyFile(keyPath, std::ios::binary);
        if (!keyFile) {
            throw std::runtime_error("Cannot read ENABLEBANKING_PRIVATE_KEY_PATH: " + keyPath);
        }
        return std::string((std::istreambuf_iterator<char>(keyFile)), std::istreambuf_iterator<char>());
    }

    const std::string pem = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PEM");
    if (!pem.empty()) {
        return pem;
    }
    const std::string pemB64 = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PEM_B64");
    if (!pemB64.empty()) {
        return base64Decode(pemB64);
    }
    return std::string();
}

void printUsage() {
    std::cout
            << "Usage:\n"
            << "  BanksConnectApp --auth-url [state] [scope]\n"
            << "  BanksConnectApp [--debug] [--mock]\n\n"
            << "Environment:\n"
            << "  PORT\n"
            << "  ENABLEBANKING_BASE_URL\n"
            << "  ENABLEBANKING_CLIENT_ID\n"
            << "  ENABLEBANKING_CLIENT_SECRET\n"
            << "  ENABLEBANKING_REDIRECT_URI\n"
            << "  PUBLIC_BASE_URL\n"
            << "  AWS_PUBLIC_BASE_URL\n"
            << "  AWS_PUBLIC_URL\n"
            << "  ENABLEBANKING_ACCESS_TOKEN\n"
            << "  ENABLEBANKING_API_TOKEN\n"
            << "  ENABLEBANKING_PRIVATE_KEY_PATH\n"
            << "  ENABLEBANKING_PRIVATE_KEY_PEM\n"
            << "  ENABLEBANKING_PRIVATE_KEY_PEM_B64\n"
            << "  ENABLEBANKING_APP_CODE\n"
            << "  ENABLEBANKING_JWT_ISSUER\n"
            << "  ENABLEBANKING_JWT_AUDIENCE\n"
            << "  ENABLEBANKING_JWT_TTL_SECONDS\n"
            << "  ENABLEBANKING_ENFORCE_HTTPS\n"
            << "  ENABLEBANKING_ADD_HSTS\n"
            << "  ENABLEBANKING_ALLOW_INSECURE_HTTP\n"
            << "  ENABLEBANKING_WEBHOOK_SECRET\n"
            << "  ENABLEBANKING_WEBHOOK_SECRET_HEADER\n"
            << "  ENABLEBANKING_SYNC_INTERVAL_SECONDS\n"
            << "  ENABLEBANKING_HTTP_TIMEOUT_SECONDS\n"
            << "  ENABLEBANKING_RATE_LIMIT_PER_MINUTE\n"
            << "  ENABLEBANKING_AUTHORIZE_PATH\n"
            << "  ENABLEBANKING_CONSENT_PATH\n"
            << "  ENABLEBANKING_ACCOUNTS_PATH\n"
            << "  ENABLEBANKING_BALANCES_PATH\n"
            << "  ENABLEBANKING_TRANSACTIONS_PATH\n"
            << "  ENABLEBANKING_ASPSP_NAME\n"
            << "  ENABLEBANKING_ASPSP_COUNTRY\n"
            << "  ENABLEBANKING_PSU_TYPE\n"
            << "  ENABLEBANKING_CONSENT_VALID_DAYS\n"
            << "  ENABLEBANKING_DATA_DIR\n"
            << "  ENABLEBANKING_TRUST_PROXY_HEADERS (set only when behind a reverse proxy)\n";
}

EnableBankingConfig loadConfig() {
    EnableBankingConfig config;
    config.baseUrl = envOrEmpty("ENABLEBANKING_BASE_URL");
    config.clientId = envOrEmpty("ENABLEBANKING_CLIENT_ID");
    config.clientSecret = envOrEmpty("ENABLEBANKING_CLIENT_SECRET");
    config.redirectUri = envOrEmpty("ENABLEBANKING_REDIRECT_URI");
    config.webhookSecret = envOrEmpty("ENABLEBANKING_WEBHOOK_SECRET");
    config.webhookSecretHeader = envOrEmpty("ENABLEBANKING_WEBHOOK_SECRET_HEADER");
    config.accessToken = envOrEmpty("ENABLEBANKING_ACCESS_TOKEN");
    config.apiToken = envOrEmpty("ENABLEBANKING_API_TOKEN");
    config.privateKeyPath = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PATH");
    config.privateKeyPem = loadPrivateKeyPem();
    config.appCode = envOrEmpty("ENABLEBANKING_APP_CODE");
    config.jwtIssuer = envOrEmpty("ENABLEBANKING_JWT_ISSUER");
    config.jwtAudience = envOrEmpty("ENABLEBANKING_JWT_AUDIENCE");
    config.jwtTtlSeconds = envOrInt("ENABLEBANKING_JWT_TTL_SECONDS", 3600);
    config.syncIntervalSeconds = envOrInt("ENABLEBANKING_SYNC_INTERVAL_SECONDS", 300);
    config.httpTimeoutSeconds = envOrInt("ENABLEBANKING_HTTP_TIMEOUT_SECONDS", 30);
    config.rateLimitPerMinute = envOrInt("ENABLEBANKING_RATE_LIMIT_PER_MINUTE", 60);
    config.enforceHttps = envOrBool("ENABLEBANKING_ENFORCE_HTTPS", true);
    config.addHsts = envOrBool("ENABLEBANKING_ADD_HSTS", true);
    config.allowInsecureHttp = envOrBool("ENABLEBANKING_ALLOW_INSECURE_HTTP", false);
    config.trustProxyHeaders = envOrBool("ENABLEBANKING_TRUST_PROXY_HEADERS", false);
    config.authorizePath = envOrEmpty("ENABLEBANKING_AUTHORIZE_PATH");
    config.consentPath = envOrEmpty("ENABLEBANKING_CONSENT_PATH");
    config.accountsPath = envOrEmpty("ENABLEBANKING_ACCOUNTS_PATH");
    config.balancesPath = envOrEmpty("ENABLEBANKING_BALANCES_PATH");
    config.transactionsPath = envOrEmpty("ENABLEBANKING_TRANSACTIONS_PATH");

    if (config.baseUrl.empty()) {
        config.baseUrl = "https://api.enablebanking.com";
    }
    if (config.redirectUri.empty()) {
        const std::string publicBaseUrl = derivePublicBaseUrl();
        if (!publicBaseUrl.empty()) {
            config.redirectUri = publicBaseUrl + "/oauth/callback";
        }
    }
    if (config.jwtIssuer.empty()) {
        config.jwtIssuer = "enablebanking.com";
    }
    if (config.jwtAudience.empty()) {
        config.jwtAudience = "api.enablebanking.com";
    }
    if (config.authorizePath.empty()) {
        config.authorizePath = "/authorize";
    }
    if (config.consentPath.empty()) {
        config.consentPath = "/consents";
    }
    if (config.accountsPath.empty()) {
        config.accountsPath = "/accounts";
    }
    if (config.balancesPath.empty()) {
        config.balancesPath = "/balances";
    }
    if (config.transactionsPath.empty()) {
        config.transactionsPath = "/transactions";
    }

    // Enable Banking specific
    config.aspspName = envOrEmpty("ENABLEBANKING_ASPSP_NAME");
    config.aspspCountry = envOrEmpty("ENABLEBANKING_ASPSP_COUNTRY");
    config.psuType = envOrEmpty("ENABLEBANKING_PSU_TYPE");
    if (config.psuType.empty()) {
        config.psuType = "personal";
    }
    config.consentValidDays = envOrInt("ENABLEBANKING_CONSENT_VALID_DAYS", 90);
    config.dataDir = envOrEmpty("ENABLEBANKING_DATA_DIR");

    return config;
}
}

namespace {
AppServer* g_serverForShutdown = nullptr;

extern "C" void handleShutdownSignal(int) {
    // Only async-signal-safe work happens here; requestShutdown() closes the
    // listening socket so the accept loop can unwind and flush state normally.
    if (g_serverForShutdown != nullptr) {
        g_serverForShutdown->requestShutdown();
    }
}

void installShutdownHandler(AppServer& server) {
    g_serverForShutdown = &server;
    struct sigaction action{};
    action.sa_handler = handleShutdownSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: accept() should return EINTR
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);

    // Writing to a closed client socket must not kill the process.
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ignore, nullptr);
}
}  // namespace

class TeeStreamBuf : public std::streambuf {
public:
    TeeStreamBuf(std::streambuf* stream1, std::streambuf* stream2)
        : stream1_(stream1), stream2_(stream2) {}
protected:
    virtual int overflow(int c) override {
        if (c == EOF) {
            return !EOF;
        } else {
            int const r1 = stream1_->sputc(c);
            int const r2 = stream2_->sputc(c);
            return r1 == EOF || r2 == EOF ? EOF : c;
        }
    }
    virtual int sync() override {
        int const r1 = stream1_->pubsync();
        int const r2 = stream2_->pubsync();
        return r1 == 0 && r2 == 0 ? 0 : -1;
    }
private:
    std::streambuf* stream1_;
    std::streambuf* stream2_;
};

int main(int argc, char** argv) {
    std::ofstream logFile;
    TeeStreamBuf* teeOut = nullptr;
    TeeStreamBuf* teeErr = nullptr;
    std::streambuf* oldOut = std::cout.rdbuf();
    std::streambuf* oldErr = std::cerr.rdbuf();

    try {
        EnableBankingConfig config = loadConfig();
        
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--debug") {
                config.debugMode = true;
            } else if (std::string(argv[i]) == "--mock") {
                config.mockMode = true;
            } else {
                args.push_back(argv[i]);
            }
        }

        if (config.debugMode) {
            logFile.open("banksconnect.log", std::ios::app);
            if (logFile.is_open()) {
                teeOut = new TeeStreamBuf(oldOut, logFile.rdbuf());
                teeErr = new TeeStreamBuf(oldErr, logFile.rdbuf());
                std::cout.rdbuf(teeOut);
                std::cerr.rdbuf(teeErr);
                std::cout << "[DEBUG] Debug mode enabled. Output is logged to banksconnect.log" << std::endl;
            }
        }

        if (args.size() > 0 && args[0] == "--help") {
            printUsage();
            return 0;
        }

        if (args.size() > 0 && args[0] == "--auth-url") {
            const std::string state = args.size() > 1 ? args[1] : "railway-state";
            const std::string scope = args.size() > 2 ? args[2] : "accounts transactions balances";
            EnableBankingClient client(config);
            std::cout << client.authorizationUrl(state, scope) << std::endl;
            return 0;
        }

        AppServer server(config);
        installShutdownHandler(server);
        server.run();

        if (teeOut) {
            std::cout.rdbuf(oldOut);
            delete teeOut;
        }
        if (teeErr) {
            std::cerr.rdbuf(oldErr);
            delete teeErr;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        printUsage();
        if (teeOut) {
            std::cout.rdbuf(oldOut);
            delete teeOut;
        }
        if (teeErr) {
            std::cerr.rdbuf(oldErr);
            delete teeErr;
        }
        return 1;
    }
}

