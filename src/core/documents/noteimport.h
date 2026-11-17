#pragma once
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <atomic>
#include <memory>

namespace NoteImport {
QString officeExecutable();
bool supportsFile(const QString &path);
QVariantMap pdf(const QString &path, const QString &destination, const QString &office,
                const std::shared_ptr<std::atomic_bool> &cancel);
QVariantMap files(const QVariantList &urls, const QString &assetRoot, const QString &job,
                 qreal x, qreal y, qreal width, const QString &office,
                 const std::shared_ptr<std::atomic_bool> &cancel);
}
