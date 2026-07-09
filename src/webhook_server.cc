#include "webhook_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::string extractBody(const std::string& request) {
    const std::string delimiter = "\r\n\r\n";
    const std::size_t bodyPos = request.find(delimiter);
    if (bodyPos == std::string::npos) {
        return std::string();
    }
    return request.substr(bodyPos + delimiter.size());
}

std::size_t contentLength(const std::string& request) {
    const std::string headerName = "Content-Length:";
    const std::size_t pos = request.find(headerName);
    if (pos == std::string::npos) {
        return 0;
    }
    const std::size_t valueStart = pos + headerName.size();
    const std::size_t valueEnd = request.find("\r\n", valueStart);
    return static_cast<std::size_t>(std::stoul(request.substr(valueStart, valueEnd - valueStart)));
}
}

WebhookServer::WebhookServer(int port) : port_(port) {}

void WebhookServer::run() const {
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    int reuse = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(serverFd);
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }

    if (::listen(serverFd, 4) < 0) {
        ::close(serverFd);
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    std::cout << "Webhook server listening on port " << port_ << std::endl;

    while (true) {
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(serverFd);
            throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
        }

        std::string request;
        char buffer[4096];
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
            const std::size_t bodyLen = contentLength(request);
            const std::string body = extractBody(request);
            if (bodyLen > 0 && body.size() >= bodyLen) {
                break;
            }
        }

        const std::string body = extractBody(request);
        std::cout << "Webhook payload:\n" << body << std::endl;

        const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
        ::send(clientFd, response, sizeof(response) - 1, 0);
        ::close(clientFd);
    }
}
