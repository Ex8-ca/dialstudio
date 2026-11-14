#include "ipc.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <unistd.h>

namespace Ipc {

QString socketPath() {
    const QString overridePath = qEnvironmentVariable("DIALSTUDIO_SOCKET");
    if (!overridePath.isEmpty()) return overridePath;
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    return (runtime.isEmpty() ? QStringLiteral("/tmp/dialstudio-%1").arg(getuid()) : runtime) + "/dialstudio.sock";
}

QByteArray encode(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact) + "\n"; }

} // namespace Ipc

IpcServer::~IpcServer() {
    // Sockets are children of the server and emit disconnected() while being destroyed;
    // detach them first so that handler cannot touch the half-destroyed client list.
    for (Client *client : std::as_const(m_clients)) {
        if (client->socket) client->socket->disconnect(this);
        delete client;
    }
    m_clients.clear();
}

bool IpcServer::listen(const QString &path, QString *error) {
    {
        IpcClient probe;
        if (probe.connectTo(path, 300)) {
            if (error) *error = "another dialstudio daemon is already running (" + path + ")";
            return false;
        }
    }
    QLocalServer::removeServer(path);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen(path)) {
        if (error) *error = QStringLiteral("cannot listen on %1: %2").arg(path, m_server.errorString());
        return false;
    }
    connect(&m_server, &QLocalServer::newConnection, this, &IpcServer::accept);
    return true;
}

void IpcServer::accept() {
    while (QLocalSocket *socket = m_server.nextPendingConnection()) {
        auto *client = new Client;
        client->socket = socket;
        m_clients << client;
        connect(socket, &QLocalSocket::readyRead, this, [this, client] { process(client); });
        connect(socket, &QLocalSocket::disconnected, this, [this, client, socket] {
            m_clients.removeAll(client);
            delete client;
            socket->deleteLater();
        });
    }
}

void IpcServer::process(Client *client) {
    client->buffer += client->socket->readAll();
    int nl;
    while ((nl = client->buffer.indexOf('\n')) >= 0) {
        const QByteArray line = client->buffer.left(nl);
        client->buffer.remove(0, nl + 1);
        if (line.trimmed().isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        QPointer<QLocalSocket> socket = client->socket;
        auto respond = [socket](const QJsonObject &reply) {
            if (socket) socket->write(Ipc::encode(reply));
        };
        if (!doc.isObject()) {
            respond({{"ok", false}, {"error", "request is not a JSON object: " + parseError.errorString()}});
            continue;
        }
        const QJsonObject request = doc.object();
        const QJsonValue id = request.value("id");
        auto withId = [respond, id](QJsonObject reply) {
            if (!id.isUndefined()) reply.insert("id", id);
            respond(reply);
        };
        if (request.value("op").toString() == "subscribe") {
            client->subscribed = true;
            withId({{"ok", true}});
            continue;
        }
        if (m_handler) m_handler(request, withId);
        else withId({{"ok", false}, {"error", "no handler"}});
    }
}

void IpcServer::broadcast(const QJsonObject &event) {
    const QByteArray line = Ipc::encode(event);
    for (Client *client : m_clients)
        if (client->subscribed && client->socket) client->socket->write(line);
}

int IpcServer::subscribers() const {
    int n = 0;
    for (const Client *c : m_clients) n += c->subscribed;
    return n;
}

bool IpcClient::connectTo(const QString &path, int timeoutMs) {
    m_socket.connectToServer(path);
    return m_socket.waitForConnected(timeoutMs);
}

void IpcClient::disconnect() {
    if (m_socket.state() != QLocalSocket::UnconnectedState) m_socket.disconnectFromServer();
}

bool IpcClient::readLine(QJsonObject *out, int timeoutMs, bool *closed) {
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        const int nl = m_buffer.indexOf('\n');
        if (nl >= 0) {
            const QByteArray line = m_buffer.left(nl);
            m_buffer.remove(0, nl + 1);
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) { *out = doc.object(); return true; }
            continue;
        }
        int wait = -1;
        if (timeoutMs >= 0) {
            wait = timeoutMs - int(timer.elapsed());
            if (wait <= 0) return false;
        }
        if (!m_socket.waitForReadyRead(wait)) {
            if (closed && m_socket.state() == QLocalSocket::UnconnectedState) *closed = true;
            return false;
        }
        m_buffer += m_socket.readAll();
    }
}

QJsonObject IpcClient::request(const QJsonObject &req, int timeoutMs, QString *error) {
    QJsonObject withId = req;
    withId.insert("id", 1);
    m_socket.write(Ipc::encode(withId));
    m_socket.flush();
    QElapsedTimer timer;
    timer.start();
    QJsonObject reply;
    while (readLine(&reply, qMax(1, timeoutMs - int(timer.elapsed())))) {
        if (reply.contains("id")) return reply;    // events carry no id
    }
    if (error) *error = "no reply from the daemon";
    return {};
}
