#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

inline QVariantMap pilonLocalDevice()
{
    return {{"id", "local"}, {"name", QCoreApplication::translate("RemoteFileManager", "This device")},
            {"ip", "127.0.0.1"}, {"online", true}, {"local", true},
            {"username", QFileInfo(QDir::homePath()).owner()}};
}
