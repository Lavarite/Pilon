#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>

namespace GitCredentials {
struct Result {
    bool ok = false;
    QByteArray output;
    QString error;
};
QByteArray request(const QString &url, const QString &username, const QString &password, QString *error);
Result execute(const QString &action, const QByteArray &request);
int runHelper(const QString &action, QIODevice &input, QIODevice &output, QIODevice &errors);
}
