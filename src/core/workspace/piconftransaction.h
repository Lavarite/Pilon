#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace PiconfTransaction {
using Files = QMap<QString, QByteArray>;
inline constexpr auto StartupPending = ".pilon/piconf-startup-pending";
bool setStartupPending(const QString &root, bool pending, QString &error);
// An absent expected entry means the destination must not exist. Extra expected
// entries are checked for concurrent edits, but are never removed.
bool commit(const QString &root, const Files &expected, const Files &updated, QString &error);
bool recover(const QString &root, QString &error);
bool snapshotEditable(const QString &root, const QStringList &paths, Files &files, QString &error);
}
