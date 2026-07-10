#ifndef BANKSCONNECTAPP_APP_SERVER_H
#define BANKSCONNECTAPP_APP_SERVER_H

#include "acc.h"
#include "enablebanking_client.h"
#include "trans.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AppServer {
public:
    explicit AppServer(EnableBankingConfig config);
    ~AppServer();

    void run();

private:
    struct HttpRequest {
        std::string method;
        std::string target;
        std::string path;
        std::string query;
        std::map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse {
        int status = 200;
        std::string contentType = "text/plain; charset=utf-8";
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    EnableBankingConfig config_;
    EnableBankingClient client_;
    std::atomic<bool> running_;
    std::thread syncThread_;
    mutable std::mutex stateMutex_;
    std::vector<acc> accounts_;
    std::vector<trans> transactions_;
    std::string expectedAuthState_;
    std::string lastAuthCode_;
    std::string lastWebhookPayload_;
    std::string lastSyncSummary_;
    std::string lastError_;

    void startSyncLoop();
    void syncOnce(const std::string& reason);
    void serve(int port);
    HttpResponse handleRequest(const HttpRequest& request);
    HttpResponse redirectToHttps(const HttpRequest& request) const;
    HttpResponse unauthorized(const std::string& message) const;
    HttpResponse jsonResponse(int status, const std::string& body) const;
    bool requestIsSecure(const HttpRequest& request) const;
    bool apiAuthorized(const HttpRequest& request) const;
    std::string configuredPublicBaseUrl() const;
    std::string buildHttpsUrl(const HttpRequest& request) const;
    std::string generateStateToken() const;
    static HttpRequest parseRequest(const std::string& rawRequest);
    static std::string readRequest(int clientFd);
    static std::string buildResponse(const HttpResponse& response);
    static std::string decodeUrl(const std::string& value);
    static std::map<std::string, std::string> parseQuery(const std::string& query);
    static std::string htmlEscape(const std::string& value);
    static bool constantTimeEquals(const std::string& left, const std::string& right);
    static std::string jsonEscape(const std::string& value);
    static std::string jsonString(const std::string& value);
    static std::string enumToString(my::currency currency);
    static std::string enumToString(my::type type);
    static std::string enumToString(my::tag tag);
    std::string renderStatus() const;
    std::string renderHome() const;
    bool webhookSecretValid(const HttpRequest& request) const;
    std::string renderAccountsJson() const;
    std::string renderTransactionsJson() const;
};

#endif //BANKSCONNECTAPP_APP_SERVER_H
