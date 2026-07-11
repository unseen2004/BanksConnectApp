#include "app_server.h"
#include "enablebanking_client.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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

std::string materializePrivateKeyPath() {
    const std::string existingPath = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PATH");
    if (!existingPath.empty()) {
        return existingPath;
    }

    std::string pem = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PEM");
    if (pem.empty()) {
        const std::string pemB64 = envOrEmpty("ENABLEBANKING_PRIVATE_KEY_PEM_B64");
        if (!pemB64.empty()) {
            pem = base64Decode(pemB64);
        }
    }
    if (pem.empty()) {
        return std::string();
    }

    char path[] = "/tmp/enablebanking-private-key-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        throw std::runtime_error("Failed to create temporary private key file");
    }
    const ssize_t written = ::write(fd, pem.data(), pem.size());
    if (written != static_cast<ssize_t>(pem.size())) {
        ::close(fd);
        throw std::runtime_error("Failed to write temporary private key file");
    }
    ::close(fd);
    ::chmod(path, 0600);
    return std::string(path);
}

void printUsage() {
    std::cout
            << "Usage:\n"
            << "  BanksConnectApp --auth-url [state] [scope]\n"
            << "  BanksConnectApp\n\n"
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
            << "  ENABLEBANKING_AUTHORIZE_PATH\n"
            << "  ENABLEBANKING_CONSENT_PATH\n"
            << "  ENABLEBANKING_ACCOUNTS_PATH\n"
            << "  ENABLEBANKING_BALANCES_PATH\n"
            << "  ENABLEBANKING_TRANSACTIONS_PATH\n"
            << "  ENABLEBANKING_ASPSP_NAME\n"
            << "  ENABLEBANKING_ASPSP_COUNTRY\n"
            << "  ENABLEBANKING_PSU_TYPE\n"
            << "  ENABLEBANKING_CONSENT_VALID_DAYS\n";
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
    config.privateKeyPath = materializePrivateKeyPath();
    config.appCode = envOrEmpty("ENABLEBANKING_APP_CODE");
    config.jwtIssuer = envOrEmpty("ENABLEBANKING_JWT_ISSUER");
    config.jwtAudience = envOrEmpty("ENABLEBANKING_JWT_AUDIENCE");
    config.jwtTtlSeconds = envOrInt("ENABLEBANKING_JWT_TTL_SECONDS", 3600);
    config.syncIntervalSeconds = envOrInt("ENABLEBANKING_SYNC_INTERVAL_SECONDS", 300);
    config.enforceHttps = envOrBool("ENABLEBANKING_ENFORCE_HTTPS", true);
    config.addHsts = envOrBool("ENABLEBANKING_ADD_HSTS", true);
    config.allowInsecureHttp = envOrBool("ENABLEBANKING_ALLOW_INSECURE_HTTP", false);
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

    return config;
}
}

int main(int argc, char** argv) {
    try {
        const EnableBankingConfig config = loadConfig();

        if (argc > 1 && std::string(argv[1]) == "--help") {
            printUsage();
            return 0;
        }

        if (argc > 1 && std::string(argv[1]) == "--auth-url") {
            const std::string state = argc > 2 ? argv[2] : "railway-state";
            const std::string scope = argc > 3 ? argv[3] : "accounts transactions balances";
            EnableBankingClient client(config);
            std::cout << client.authorizationUrl(state, scope) << std::endl;
            return 0;
        }

        AppServer server(config);
        server.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        printUsage();
        return 1;
    }
}
