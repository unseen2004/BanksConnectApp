#ifndef BANKSCONNECTAPP_WEBHOOK_SERVER_H
#define BANKSCONNECTAPP_WEBHOOK_SERVER_H

class WebhookServer {
public:
    explicit WebhookServer(int port);
    void run() const;

private:
    int port_;
};

#endif //BANKSCONNECTAPP_WEBHOOK_SERVER_H
