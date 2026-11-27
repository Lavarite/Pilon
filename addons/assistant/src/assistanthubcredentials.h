#pragma once

#include <QByteArray>
#include <QString>

namespace AssistantHubCredentials {
struct Result {
    bool ok = false;
    QByteArray token;
    QString error;
};

Result lookup();
Result store(const QByteArray &token);
Result clear();
}
