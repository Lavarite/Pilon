#include "gitcredentials.h"
#include "../../../src/platform/android/androidcredentials.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace {
constexpr qsizetype maximumRequest = 65536;
bool plain(const QByteArray &value) {
    for (const auto ch : value) if (static_cast<unsigned char>(ch) < 32 || ch == 127) return false;
    return QString::fromUtf8(value).toUtf8() == value;
}
QString endpoint(const QMap<QByteArray, QByteArray> &fields) {
    if (fields.value("protocol") != "https") return {};
    const auto host = fields.value("host");
    if (host.isEmpty()) return {};
    QUrl url(QString::fromUtf8("https://" + host), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || !url.path().isEmpty() || url.hasQuery() || url.hasFragment()) return {};
    const auto path = fields.value("path");
    if (path.startsWith('/') || path.contains('\\')) return {};
    for (const auto &part : path.split('/')) {
        if (part == "." || part == ".." || !plain(part)) return {};
    }
    return url.toString(QUrl::FullyEncoded) + (path.isEmpty() ? QString() : "/" + QString::fromUtf8(path));
}
GitCredentials::Result failure(const QString &message) { return {false, {}, message}; }
GitCredentials::Result keyring(const QString &action, const QString &scope, const QByteArray &username, const QByteArray &secret = {}) {
#ifdef Q_OS_ANDROID
    const auto result = AndroidCredentials::request(action,
        {{"application", "pilon-git"}, {"endpoint", scope}, {"username", QString::fromUtf8(username)}}, secret);
    return result.ok ? GitCredentials::Result{true, result.secret, {}}
        : failure(QStringLiteral("The Android credential store could not complete this request."));
#endif
    const auto tool = QStandardPaths::findExecutable("secret-tool");
    if (tool.isEmpty()) return failure(QStringLiteral("Install secret-tool (libsecret) to use saved Git credentials."));
    QStringList arguments{action};
    if (action == "store") arguments << "--label=Pilon Git account";
    arguments << "application" << "pilon-git" << "endpoint" << scope << "username" << QString::fromUtf8(username);
    QProcess process;
    QByteArray output;
    qsizetype errorBytes = 0;
    bool oversized = false;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, [&] {
        const auto bytes = process.readAllStandardOutput();
        if (output.size() + bytes.size() > maximumRequest) { oversized = true; process.kill(); }
        else output += bytes;
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&] {
        errorBytes += process.readAllStandardError().size();
        if (errorBytes > maximumRequest) { oversized = true; process.kill(); }
    });
    process.start(tool, arguments);
    if (!process.waitForStarted(3000)) return failure(QStringLiteral("The system keyring could not be opened."));
    if (action == "store") process.write(secret);
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning && !process.waitForFinished(10000)) {
        process.kill(); process.waitForFinished(1000);
        return failure(QStringLiteral("The system keyring did not respond. Unlock it and try again."));
    }
    if (oversized) return failure(QStringLiteral("The system keyring returned too much data. Check secret-tool and try again."));
    const bool absent = (action == "lookup" || action == "clear") && process.exitCode() == 1 && errorBytes == 0;
    if (process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && !absent))
        return failure(QStringLiteral("The system keyring request failed. Unlock it and try again."));
    return {true, absent ? QByteArray() : output.trimmed(), {}};
}
}

QByteArray GitCredentials::request(const QString &address, const QString &username, const QString &password, QString *error) {
    const QUrl url(address.trimmed(), QUrl::StrictMode);
    if (error) error->clear();
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment() || !plain(username.toUtf8()) || !plain(password.toUtf8())) {
        if (error) *error = QStringLiteral("Enter an HTTPS repository URL without credentials, a username and a token.");
        return {};
    }
    auto path = url.path(QUrl::FullyDecoded); if (path.startsWith('/')) path.remove(0, 1);
    QByteArray data = "protocol=https\nhost=" + url.authority(QUrl::FullyEncoded).toUtf8() + '\n';
    if (!path.isEmpty()) data += "path=" + path.toUtf8() + '\n';
    if (!username.isEmpty()) data += "username=" + username.toUtf8() + '\n';
    if (!password.isEmpty()) data += "password=" + password.toUtf8() + '\n';
    data += '\n';
    QMap<QByteArray,QByteArray> fields{{"protocol", "https"}, {"host", url.authority(QUrl::FullyEncoded).toUtf8()}, {"path", path.toUtf8()}};
    if (data.size() > maximumRequest || endpoint(fields).isEmpty()) {
        if (error) *error = QStringLiteral("Enter a valid HTTPS repository URL and shorter credentials.");
        return {};
    }
    return data;
}

GitCredentials::Result GitCredentials::execute(const QString &action, const QByteArray &data) {
    if ((action != "get" && action != "store" && action != "erase") || data.size() > maximumRequest)
        return failure(QStringLiteral("Invalid Git credential request."));
    QMap<QByteArray, QByteArray> fields;
    bool ended = false;
    for (const auto &line : data.split('\n')) {
        if (line.isEmpty()) { ended = true; continue; }
        const auto separator = line.indexOf('=');
        if (ended || separator <= 0 || !plain(line)) return failure(QStringLiteral("Invalid Git credential request."));
        const auto key = line.first(separator), value = line.sliced(separator + 1);
        if (key != "protocol" && key != "host" && key != "path" && key != "username" && key != "password") continue;
        if (fields.contains(key)) return failure(QStringLiteral("Invalid Git credential request."));
        fields.insert(key, value);
    }
    const auto scope = endpoint(fields);
    if (scope.isEmpty()) return failure(QStringLiteral("Saved Git credentials require a valid HTTPS endpoint."));
    if (action == "store") {
        if (fields.value("username").isEmpty() || fields.value("password").isEmpty())
            return failure(QStringLiteral("Enter a Git username and token."));
        const auto secret = QJsonDocument(QJsonObject{{"username", QString::fromUtf8(fields.value("username"))},
                                                     {"password", QString::fromUtf8(fields.value("password"))}}).toJson(QJsonDocument::Compact);
        auto result = keyring("store", scope, fields.value("username"), secret); result.output.clear(); return result;
    }
    if (fields.value("username").isEmpty()) return {true, {}, {}};
    auto result = keyring("lookup", scope, fields.value("username"));
    if (!result.ok || result.output.isEmpty()) return result;
    QJsonParseError parseError;
    const auto object = QJsonDocument::fromJson(result.output, &parseError).object();
    const auto username = object.value("username").toString().toUtf8(), password = object.value("password").toString().toUtf8();
    if (parseError.error != QJsonParseError::NoError || username.isEmpty() || password.isEmpty() || !plain(username) || !plain(password))
        return failure(QStringLiteral("The saved Git credential is invalid. Save it again."));
    if (fields.contains("username") && fields.value("username") != username) return {true, {}, {}};
    if (action == "erase") { result = keyring("clear", scope, fields.value("username")); result.output.clear(); return result; }
    return {true, "username=" + username + "\npassword=" + password + "\n\n", {}};
}

int GitCredentials::runHelper(const QString &action, QIODevice &input, QIODevice &output, QIODevice &errors) {
    QByteArray data;
    while (!input.atEnd()) {
        const auto line = input.readLine(maximumRequest + 2);
        data += line;
        if (data.size() > maximumRequest || line == "\n") break;
    }
    const auto result = execute(action, data);
    if (!result.ok) { errors.write(result.error.toUtf8() + '\n'); return 1; }
    output.write(result.output); return 0;
}
