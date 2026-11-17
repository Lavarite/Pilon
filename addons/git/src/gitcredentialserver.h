#pragma once
#include "gitcredentials.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QtEndian>
#include <functional>
#include <memory>

class GitCredentialServer : public QObject {
public:
    using Completed = std::function<void(GitCredentials::Result)>;
    using Handler = std::function<void(QString, QByteArray, Completed)>;

    GitCredentialServer(const QString &path, Handler handler, QObject *parent = nullptr)
        : QObject(parent), m_handler(std::move(handler)) {
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(4);
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (auto *socket = m_server.nextPendingConnection()) accept(socket);
        });
        m_server.listen(path);
    }
    QString address() const { return m_server.isListening() ? m_server.fullServerName() : QString{}; }
    void clearScope() { m_scope.clear(); m_provider.clear(); m_host.clear(); }
    bool setScope(const QByteArray &scope) {
        if (m_handling || scope.isEmpty()) return false;
        m_scope = scope;
        m_provider.clear(); m_host.clear();
        return true;
    }
    bool setProviderScope(const QString &provider, const QString &host) {
        const QUrl url("https://" + host);
        if (m_handling || (provider != "gh" && provider != "glab") || host.isEmpty()
            || !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
            || !url.path().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
        m_scope.clear(); m_provider = provider; m_host = host;
        return true;
    }

private:
    static bool matchesScope(const QByteArray &scope, const QByteArray &request) {
        const auto fields = [](const QByteArray &data) {
            QMap<QByteArray, QByteArray> result;
            for (const auto &line : data.split('\n')) {
                const auto separator = line.indexOf('=');
                if (separator > 0) result.insert(line.first(separator), line.sliced(separator + 1));
            }
            return result;
        };
        const auto expected = fields(scope), actual = fields(request);
        if (expected.value("protocol") != "https" || expected.value("host").isEmpty()
            || expected.value("username").isEmpty()) return false;
        for (const auto *key : {"protocol", "host", "path", "username"})
            if (actual.value(key) != expected.value(key)) return false;
        return true;
    }
    static void reply(QLocalSocket *socket, const GitCredentials::Result &result) {
        QByteArray frame(5, '\0');
        const bool ok = result.ok && result.output.size() <= 65536;
        frame[0] = ok ? 1 : 0;
        qToBigEndian<quint32>(ok ? result.output.size() : 0, frame.data() + 1);
        if (ok) frame += result.output;
        socket->write(frame);
        frame.fill('\0');
        socket->disconnectFromServer();
    }
    bool matchesProviderScope(const QByteArray &request) const {
        const auto object = QJsonDocument::fromJson(request).object();
        if (m_provider.isEmpty() || object.size() != 5 || !object.value("action").isString()
            || !object.value("service").isString() || !object.value("user").isString()
            || !object.value("secret").isString() || !object.value("all").isBool()) return false;
        const auto action = object.value("action").toString();
        const auto service = object.value("service").toString();
        if ((action != "lookup" && action != "store" && action != "clear")
            || (object.value("all").toBool() && action != "clear")
            || object.value("user").toString().size() > 1024) return false;
        if (m_provider == "gh")
            return service == "gh:" + m_host || service == "gh:_refreshable:" + m_host;
        if (!object.value("user").toString().isEmpty()) return false;
        if (service == "glab:" + m_host || service == "glab:" + m_host + ":token"
            || service == "glab:" + m_host + ":job_token"
            || service == "glab:" + m_host + ":oauth2_refresh_token") return true;
        const auto probe = service.split(':');
        bool pid = false, sequence = false;
        if (probe.size() == 4 && probe[0] == "glab" && probe[1] == "__keyring_probe__") {
            probe[2].toULongLong(&pid); probe[3].toULongLong(&sequence);
        }
        return pid && sequence;
    }
    void accept(QLocalSocket *socket) {
        socket->setReadBufferSize(65557);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(10000, socket, [socket] { socket->abort(); });
        auto buffer = std::make_shared<QByteArray>();
        auto handled = std::make_shared<bool>(false);
        connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer, handled] {
            if (*handled) { socket->abort(); return; }
            *buffer += socket->readAll();
            if (buffer->size() < 4) return;
            const auto length = qFromBigEndian<quint32>(buffer->constData());
            if (length > 65552 || buffer->size() > length + 4) {
                *handled = true;
                buffer->fill('\0'); buffer->clear();
                reply(socket, {}); return;
            }
            if (buffer->size() < length + 4) return;
            *handled = true;
            const auto separator = buffer->indexOf('\n', 4);
            const auto action = buffer->mid(4, separator - 4);
            auto request = buffer->mid(separator + 1);
            buffer->fill('\0'); buffer->clear();
            const bool permitted = action == "keyring" ? matchesProviderScope(request)
                : ((action == "get" || action == "store" || action == "erase") && matchesScope(m_scope, request));
            if (separator < 4 || m_handling || !permitted) { reply(socket, {}); return; }
            if (action == "keyring") {
                auto object = QJsonDocument::fromJson(request).object();
                object.insert("_provider", m_provider); object.insert("_host", m_host);
                request = QJsonDocument(object).toJson(QJsonDocument::Compact);
            }
            m_handling = true;
            const QPointer<GitCredentialServer> alive(this);
            const QPointer<QLocalSocket> connection(socket);
            m_handler(QString::fromLatin1(action), std::move(request), [alive, connection](GitCredentials::Result result) {
                if (alive) alive->m_handling = false;
                if (alive && connection) reply(connection, result);
                result.output.fill('\0');
            });
        });
    }
    QLocalServer m_server;
    Handler m_handler;
    QByteArray m_scope;
    QString m_provider, m_host;
    bool m_handling = false;
};
