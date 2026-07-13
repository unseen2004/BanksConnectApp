#ifndef BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H
#define BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H

#include <string>
#include <vector>

struct EnableBankingConfig {
    std::string baseUrl;
    std::string clientId;      // kept for backward compat, unused by EB API
    std::string clientSecret;  // kept for backward compat, unused by EB API
    std::string redirectUri;
    std::string webhookSecret;
    std::string webhookSecretHeader;
    std::string accessToken;
    std::string apiToken;
    std::string privateKeyPath;
    std::string appCode;
    std::string jwtIssuer;
    std::string jwtAudience;
    int jwtTtlSeconds = 3600;
    int syncIntervalSeconds = 300;
    bool enforceHttps = true;
    bool addHsts = true;
    bool allowInsecureHttp = false;
    std::string authorizePath;
    std::string consentPath;
    std::string accountsPath;
    std::string balancesPath;
    std::string transactionsPath;

    // Enable Banking specific
    std::string aspspName;     // e.g. "Nordea", "mBank"
    std::string aspspCountry;  // e.g. "FI", "PL"
    std::string psuType;       // "personal" or "business"
    int consentValidDays = 90; // how many days the consent is valid
};

struct HttpResponse {
    long statusCode = 0;
    std::string body;
};

/// Result of POST /auth — contains the URL to redirect the PSU to.
struct StartAuthResult {
    std::string url;              // URL to redirect the user to
    std::string authorizationId;  // authorization_id from Enable Banking
};

/// Result of POST /sessions — contains session_id and account IDs.
struct SessionResult {
    std::string sessionId;
    std::vector<std::string> accountIds;  // account UUIDs from EB
    std::string rawJson;                  // full response for debugging
};

class EnableBankingClient {
public:
    explicit EnableBankingClient(EnableBankingConfig config);

    // Legacy — kept for backward compat / CLI mode
    std::string authorizationUrl(const std::string& state, const std::string& scope) const;

    // ===== Enable Banking proper flow =====

    /// Step 1: POST /auth — start authorization, returns redirect URL for the PSU.
    StartAuthResult startAuthorization(const std::string& state) const;

    /// Overload: start authorization with a specific ASPSP (for multi-bank support).
    StartAuthResult startAuthorization(const std::string& state,
                                       const std::string& aspspName,
                                       const std::string& aspspCountry) const;

    /// Step 2: POST /sessions — exchange the callback code for a session.
    SessionResult createSession(const std::string& code) const;

    /// Fetch account balances for a specific account.
    HttpResponse getAccountBalances(const std::string& accountId) const;

    /// Fetch account transactions for a specific account.
    HttpResponse getAccountTransactions(const std::string& accountId) const;

    /// Fetch account details for a specific account.
    HttpResponse getAccountDetails(const std::string& accountId) const;

    // Legacy generic endpoints (still available)
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
    static std::string jsonEscapeStatic(const std::string& value);
};

#endif //BANKSCONNECTAPP_ENABLEBANKING_CLIENT_H
