// Newline-delimited JSON over a local socket: server for the daemon, blocking
// client for the CLI (and, later, the GUI).
#pragma once

#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <functional>

namespace Ipc {
QString socketPath();
QByteArray encode(const QJsonObject &object);
}

using IpcResponder = std::function<void(const QJsonObject &)>;
using IpcHandler = std::function<void(const QJsonObject &request, const IpcResponder &respond)>;

class IpcServer : public QObject {
    Q_OBJECT
public:
    explicit IpcServer(QObject *parent = nullptr) : QObject(parent) {}
    ~IpcServer() override;
    // Fails if another daemon already answers on the socket; removes a stale socket file otherwise.
    bool listen(const QString &path, QString *error);
    void setHandler(IpcHandler handler) { m_handler = std::move(handler); }
    void broadcast(const QJsonObject &event);
    int subscribers() const;

private:
    struct Client {
        QPointer<QLocalSocket> socket;
        QByteArray buffer;
        bool subscribed = false;
    };
    void accept();
    void process(Client *client);

    QLocalServer m_server;
    QList<Client *> m_clients;
    IpcHandler m_handler;
};

class IpcClient {
public:
    ~IpcClient() { disconnect(); }
    bool connectTo(const QString &path, int timeoutMs = 1000);
    void disconnect();
    // Sends one request and waits for the reply. Events interleaved before it are skipped.
    QJsonObject request(const QJsonObject &request, int timeoutMs = 3000, QString *error = nullptr);
    // Blocks for the next line; false on timeout (timeoutMs >= 0) or disconnect.
    bool readLine(QJsonObject *out, int timeoutMs, bool *closed = nullptr);

private:
    QLocalSocket m_socket;
    QByteArray m_buffer;
};
