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
#include <utility>
#include <vector>

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
    int stdinPipe[2];
    int stdoutPipe[2];
    if (::pipe(stdinPipe) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }
    if (::pipe(stdoutPipe) != 0) {
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        throw std::runtime_error(std::string("pipe failed: ") + std::strerror(errno));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        throw std::runtime_error(std::string("fork failed: ") + std::strerror(errno));
    }

    if (pid == 0) {
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        ::dup2(stdinPipe[0], STDIN_FILENO);
        ::dup2(stdoutPipe[1], STDOUT_FILENO);
        ::close(stdinPipe[0]);
        ::close(stdoutPipe[1]);

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("openssl"));
        argv.push_back(const_cast<char*>("dgst"));
        argv.push_back(const_cast<char*>("-sha256"));
        argv.push_back(const_cast<char*>("-sign"));
        argv.push_back(const_cast<char*>(privateKeyPath.c_str()));
        argv.push_back(nullptr);
        ::execvp("openssl", argv.data());
        _exit(127);
    }

    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);
    const ssize_t written = ::write(stdinPipe[1], message.data(), message.size());
    (void)written;
    ::close(stdinPipe[1]);

    const std::string signature = readAll(stdoutPipe[0]);
    ::close(stdoutPipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed: ") + std::strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("openssl signing failed");
    }

    return signature;
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
    if (config_.clientId.empty() || config_.redirectUri.empty()) {
        throw std::runtime_error("ENABLEBANKING_CLIENT_ID and ENABLEBANKING_REDIRECT_URI are required");
    }
    return makeUrl(config_.authorizePath) + "?response_type=code&client_id=" + urlEncode(config_.clientId) +
           "&redirect_uri=" + urlEncode(config_.redirectUri) + "&scope=" + urlEncode(scope) +
           "&state=" + urlEncode(state);
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
