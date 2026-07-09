#ifndef BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H
#define BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H

#include <string>

struct EnableBankingConfig {
    std::string baseUrl;
    std::string clientId;
    std::string clientSecret;
    std::string redirectUri;
    std::string webhookSecret;
    std::string webhookSecretHeader;
    std::string accessToken;
    std::string privateKeyPath;
    std::string appCode;
    std::string jwtIssuer;
    std::string jwtAudience;
    int jwtTtlSeconds = 3600;
    int syncIntervalSeconds = 300;
    std::string authorizePath;
    std::string consentPath;
    std::string accountsPath;
    std::string balancesPath;
    std::string transactionsPath;
};

struct HttpResponse {
    long statusCode = 0;
    std::string body;
};

class EnableBankingClient {
public:
    explicit EnableBankingClient(EnableBankingConfig config);

    std::string authorizationUrl(const std::string& state, const std::string& scope) const;
    HttpResponse createConsent(const std::string& consentPayloadJson) const;
    HttpResponse getAccounts() const;
    HttpResponse getBalances() const;
    HttpResponse getTransactions() const;
    HttpResponse get(const std::string& path) const;
    HttpResponse post(const std::string& path, const std::string& body) const;

private:
    EnableBankingConfig config_;

    HttpResponse request(const std::string& method, const std::string& path, const std::string& body) const;
    std::string bearerToken() const;
    std::string generateJwt() const;
    std::string makeUrl(const std::string& path) const;
    static std::string urlEncode(const std::string& value);
    static std::string base64UrlEncode(const unsigned char* data, std::size_t size);
    static std::string base64UrlEncode(const std::string& value);
    static std::string signWithOpenSsl(const std::string& message, const std::string& privateKeyPath);
};

#endif //BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H
