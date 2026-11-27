#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTcpServer>
#include <functional>

class QNetworkReply;
class QTcpSocket;

// Local transport adapter only: Codex still decides when to call tools and
// continue. No prompts, tool execution, or agent loop live in this adapter.
class CodexProvider : public QObject
{
    Q_OBJECT
public:
    explicit CodexProvider(QObject *parent = nullptr);
    ~CodexProvider() override;
    bool listen();
    quint16 port() const { return m_server.serverPort(); }
    QByteArray token() const { return m_token; }
    void configure(quint16 backendPort, const QByteArray &backendKey, const QJsonObject &parameters, bool liteRt = false);
    void configureRemote(const QVariantMap &profile, const QByteArray &key);
    void complete(const QJsonObject &body);
    void cancel();
    void pauseForRetry();
    void retry();
    bool active() const { return !m_socket.isNull(); }
    static QJsonObject chatRequest(const QJsonObject &response, QString *error);
    static bool contextOverflow(const QString &message);
    static QJsonArray responseHistory(const QJsonArray &messages);
signals:
    void requested(const QJsonObject &body);
    void progress(int cached, int processed, int total);
    void activity();
    void rateLimits(const QVariantMap &profile, const QVariantMap &limits);
    void tokensUsed(int cached, int input, int output);
    void toolDelta(int index, const QJsonObject &call);
    void cancelled();
    void backendFailed(const QString &message);
private:
    void accept();
    void consume();
    void finish();
    void event(QJsonObject value);
    void failure(const QString &message);
    void delta(const QString &text, bool reasoning);
    void endText(bool reasoning);
    QTcpServer m_server;
    QNetworkAccessManager m_network;
    QPointer<QTcpSocket> m_socket;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_token;
    QByteArray m_backendKey;
    QUrl m_backendUrl;
    QVariantMap m_remoteProfile;
    bool m_remote = false;
    QJsonObject m_parameters;
    QJsonObject m_pendingBody;
    QByteArray m_buffer;
    QJsonArray m_output;
    QJsonArray m_calls;
    QHash<QString, QPair<QString, QString>> m_toolNames;
    QSet<QString> m_customTools;
    QString m_responseId;
    QString m_text;
    QString m_reasoning;
    QString m_finishReason;
    QJsonObject m_usage;
    QJsonObject m_timings;
    int m_textIndex = -1;
    int m_reasoningIndex = -1;
    int m_cachedTokens = 0;
    qsizetype m_streamBytes = 0;
    qint64 m_sequence = 0;
    bool m_done = false;
    bool m_retrying = false;
    bool m_liteRt = false;
};
