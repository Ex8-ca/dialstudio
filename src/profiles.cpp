#include "profiles.h"

#include "config.h"

#include <QDir>
#include <QFileInfo>

Profiles::Profiles(Config *config, QObject *parent) : QObject(parent), m_config(config) {
    connect(config, &Config::changed, this, [this] { refresh(true); });
    refresh(true);
}

QString Profiles::resolve(const QJsonArray &profiles, const QString &windowClass, const QString &title) {
    QString fallback;
    for (const auto &item : profiles) {
        const QJsonObject profile = item.toObject();
        const QJsonValue match = profile.value("match");
        const QString name = profile.value("name").toString();
        if (match.isNull() || match.isUndefined()) {
            if (fallback.isEmpty()) fallback = name;
            continue;
        }
        const QJsonObject m = match.toObject();
        const QString classPattern = m.value("class").toString();
        const QString titlePattern = m.value("title").toString();
        if (!classPattern.isEmpty() && !QRegularExpression(classPattern).match(windowClass).hasMatch()) continue;
        if (!titlePattern.isEmpty() && !QRegularExpression(titlePattern).match(title).hasMatch()) continue;
        return name;
    }
    return fallback;
}

void Profiles::refresh(bool force) {
    QString next;
    if (!m_pinned.isEmpty() && m_config->profileIndex(m_pinned) >= 0) next = m_pinned;
    else {
        m_pinned.clear();
        next = resolve(m_config->profiles(), m_class, m_title);
    }
    const bool different = next != m_active;
    m_active = next;
    if (different || force) emit changed();
}

void Profiles::setActiveWindow(const QString &windowClass, const QString &title) {
    m_class = windowClass;
    m_title = title;
    refresh(false);
}

bool Profiles::pin(const QString &name) {
    if (m_config->profileIndex(name) < 0) return false;
    m_pinned = name;
    refresh(true);
    return true;
}

void Profiles::unpin() {
    m_pinned.clear();
    refresh(true);
}

QJsonObject Profiles::activeProfile() const {
    const int i = m_config->profileIndex(m_active);
    return i < 0 ? QJsonObject() : m_config->profiles()[i].toObject();
}

int Profiles::pageCount() const { return activeProfile().value("pages").toArray().size(); }

int Profiles::page() const {
    const int count = pageCount();
    return count == 0 ? 0 : qBound(0, m_pages.value(m_active, 0), count - 1);
}

QString Profiles::pageName() const {
    return activeProfile().value("pages").toArray().at(page()).toObject().value("name").toString();
}

bool Profiles::setPage(int page) {
    if (page < 0 || page >= pageCount()) return false;
    if (m_pages.value(m_active, 0) != page) {
        m_pages[m_active] = page;
        emit changed();
    }
    return true;
}

bool Profiles::nextPage(int delta) {
    const int count = pageCount();
    if (count == 0) return false;
    return setPage(((page() + delta) % count + count) % count);
}

QJsonObject Profiles::lookup(const QString &control, const QString &slot) const {
    const QJsonArray pages = activeProfile().value("pages").toArray();
    if (pages.isEmpty()) return {};
    return pages.at(page()).toObject().value("bindings").toObject().value(control).toObject().value(slot).toObject();
}

// ---------------------------------------------------------------------------

HyprlandWatcher::HyprlandWatcher(QObject *parent) : QObject(parent) {
    m_retry.setSingleShot(true);
    m_retry.setInterval(3000);
    connect(&m_retry, &QTimer::timeout, this, &HyprlandWatcher::connectNow);
    connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
        m_buffer += m_socket.readAll();
        int nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QByteArray line = m_buffer.left(nl);
            m_buffer.remove(0, nl + 1);
            QString c, t;
            if (parseLine(line, &c, &t)) emit windowChanged(c, t);
        }
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] { m_buffer.clear(); m_retry.start(); });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] { if (!m_retry.isActive()) m_retry.start(); });
}

HyprlandWatcher::~HyprlandWatcher() {
    // The socket is destroyed after the retry timer and emits disconnected() while it
    // goes; without this the handler would restart a timer that no longer exists.
    m_socket.disconnect(this);
    m_retry.stop();
}

void HyprlandWatcher::start() { connectNow(); }

void HyprlandWatcher::connectNow() {
    if (m_socket.state() != QLocalSocket::UnconnectedState) return;
    const QString path = findSocket(qEnvironmentVariable("XDG_RUNTIME_DIR"), qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE"));
    if (path.isEmpty()) { m_retry.start(); return; }
    m_socket.connectToServer(path);
}

QString HyprlandWatcher::findSocket(const QString &runtimeDir, const QString &signature, QString *signatureOut) {
    if (!signature.isEmpty()) {
        const QString path = QStringLiteral("%1/hypr/%2/.socket2.sock").arg(runtimeDir, signature);
        if (QFileInfo::exists(path)) { if (signatureOut) *signatureOut = signature; return path; }
    }
    QDir dir(runtimeDir + "/hypr");
    QString best;
    QDateTime bestTime;
    for (const auto &entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QFileInfo socket(entry.absoluteFilePath() + "/.socket2.sock");
        if (socket.exists() && (best.isEmpty() || socket.lastModified() > bestTime)) {
            best = socket.absoluteFilePath();
            bestTime = socket.lastModified();
            if (signatureOut) *signatureOut = entry.fileName();
        }
    }
    return best;
}

bool HyprlandWatcher::parseLine(const QByteArray &line, QString *windowClass, QString *title) {
    static const QByteArray prefix = "activewindow>>";
    if (!line.startsWith(prefix)) return false;
    const QString rest = QString::fromUtf8(line.mid(prefix.size()));
    // The class never contains a comma in practice; the title may.
    const int comma = rest.indexOf(',');
    *windowClass = comma < 0 ? rest : rest.left(comma);
    *title = comma < 0 ? QString() : rest.mid(comma + 1);
    return true;
}
