#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QLockFile>
#include <QtQml/qqmlregistration.h>
#include <memory>

// Pilon's private CLI installation. A running chat keeps its existing process.
class CodexRuntime : public QObject {
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY checkChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY checkChanged)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY checkChanged)
public:
    explicit CodexRuntime(QObject *parent = nullptr);
    ~CodexRuntime() override;
    void start(const QString &root, const QString &fallback);
    void stop();
    void checkNow() { check(true); }
    bool supported() const;
    bool checking() const { return m_stage != Idle; }
    QString installedVersion() const { return m_installed; }
    QString updateStatus() const { return m_status; }
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    static QString executable(const QString &root);
    static QString version(const QByteArray &output);
signals:
    void updated();
    void checkChanged();
private:
    friend class AssistantTests;
    void check(bool manual = false);
    void fetchLatest(const QString &installed);
    void finish(const QString &error = {});
    std::unique_ptr<QLockFile> m_lock;
    QNetworkAccessManager m_network;
    QProcess m_process;
    QTimer m_timer, m_timeout;
    QString m_root, m_fallback, m_candidate, m_latest, m_installed, m_status;
    bool m_manual = false;
    quint64 m_generation = 0;
    enum Stage { Idle, Version, Install, Verify } m_stage = Idle;
};
