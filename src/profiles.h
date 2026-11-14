// Profile and page resolution, plus the Hyprland active-window listener.
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QRegularExpression>
#include <QTimer>

class Config;

class Profiles : public QObject {
    Q_OBJECT
public:
    explicit Profiles(Config *config, QObject *parent = nullptr);

    void setActiveWindow(const QString &windowClass, const QString &title);
    QString windowClass() const { return m_class; }
    QString windowTitle() const { return m_title; }

    // Manual pin until unpin(). Returns false for an unknown name.
    bool pin(const QString &name);
    void unpin();
    bool pinned() const { return !m_pinned.isEmpty(); }

    QString activeName() const { return m_active; }
    int page() const;
    int pageCount() const;
    QString pageName() const;
    bool setPage(int page);
    bool nextPage(int delta);   // wraps around

    // The action bound to control/slot on the active page, or an empty object.
    QJsonObject lookup(const QString &control, const QString &slot) const;
    bool hasBinding(const QString &control, const QString &slot) const { return !lookup(control, slot).isEmpty(); }

    // Pure matcher, used by tests: first profile whose match fits, else the fallback.
    static QString resolve(const QJsonArray &profiles, const QString &windowClass, const QString &title);

signals:
    void changed();   // active profile or page changed

private:
    void refresh(bool force);
    QJsonObject activeProfile() const;

    Config *m_config;
    QString m_class, m_title;
    QString m_pinned;
    QString m_active;
    QHash<QString, int> m_pages;
};

// Listens to Hyprland's socket2 for `activewindow>>class,title`.
class HyprlandWatcher : public QObject {
    Q_OBJECT
public:
    explicit HyprlandWatcher(QObject *parent = nullptr);
    ~HyprlandWatcher() override;
    void start();
    // Finds the event socket from the environment, falling back to the newest instance
    // under $XDG_RUNTIME_DIR/hypr (a systemd user service may not inherit the signature).
    static QString findSocket(const QString &runtimeDir, const QString &signature, QString *signatureOut = nullptr);
    static bool parseLine(const QByteArray &line, QString *windowClass, QString *title);

signals:
    void windowChanged(const QString &windowClass, const QString &title);

private:
    void connectNow();
    QLocalSocket m_socket;
    QTimer m_retry;
    QByteArray m_buffer;
};
