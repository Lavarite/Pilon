#pragma once

#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <functional>

// The app-server owns threads and the agent loop. Pilon owns its UI and tools.
class CodexClient : public QObject
{
    Q_OBJECT
public:
    explicit CodexClient(QObject *parent = nullptr);
    ~CodexClient() override;
    void start(const QString &executable, const QStringList &arguments, const QProcessEnvironment &environment);
    void shutdown();
    bool ready() const { return m_ready; }
    bool running() const { return m_process.state() != QProcess::NotRunning; }
    using Reply = std::function<void(QJsonObject, QString)>;
    void request(const QString &method, const QJsonObject &params, Reply reply = {});
    void respond(const QJsonValue &id, const QJsonObject &result);
    void reject(const QJsonValue &id, const QString &message);
signals:
    void initialized();
    void notification(const QString &method, const QJsonObject &params);
    void serverRequest(const QJsonValue &id, const QString &method, const QJsonObject &params);
    void failed(const QString &message);
private:
    void write(const QJsonObject &message);
    void consume();
    QProcess m_process;
    QTimer m_startTimer;
    QByteArray m_buffer;
    QByteArray m_log;
    QHash<qint64, Reply> m_requests;
    qint64 m_nextId = 0;
    bool m_ready = false;
    bool m_stopping = false;
};
