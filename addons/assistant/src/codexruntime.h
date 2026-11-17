#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QLockFile>
#include <memory>

// Pilon's private CLI installation. A running chat keeps its existing process.
class CodexRuntime : public QObject {
    Q_OBJECT
public:
    explicit CodexRuntime(QObject *parent = nullptr);
    ~CodexRuntime() override;
    void start(const QString &root, const QString &fallback);
    void stop();
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    static QString executable(const QString &root);
    static QString version(const QByteArray &output);
signals:
    void updated();
private:
    friend class AssistantTests;
    void check();
    void fetchLatest(const QString &installed);
    void finish(const QString &error = {});
    std::unique_ptr<QLockFile> m_lock;
    QNetworkAccessManager m_network;
    QProcess m_process;
    QTimer m_timer, m_timeout;
    QString m_root, m_fallback, m_candidate, m_latest;
    quint64 m_generation = 0;
    enum Stage { Idle, Version, Install, Verify } m_stage = Idle;
};
