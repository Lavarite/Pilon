#pragma once

#include <QByteArray>
#include <QMap>
#include <QStringList>
#include <QVariantMap>
#include <memory>

// A validated, in-memory graph. Persistence and import own the write transaction.
class PiconfDocument {
  public:
    using Files = QMap<QString, QByteArray>;
    static const QString MainPath;
    static constexpr qsizetype MaxFileBytes = 1024 * 1024;
    static constexpr qsizetype MaxBytes = 8 * MaxFileBytes;
    static constexpr int MaxFiles = 256;

    static bool validPath(const QString &relative);
    static QString storagePath(const QString &relative);
    static QByteArray encode(const QVariantMap &values);
    bool load(const QString &workspace, QString &error);
    bool parse(const Files &files, QString &error);
    const Files &files() const;
    QStringList missingAddonConfigs() const;
    QString addonFile(const QString &id) const;
    QVariantMap values(const QString &path = MainPath) const;
    bool setValue(const QString &path, const QStringList &keys, const QVariant &value, QString &error);

    // Update the originating assignment in an existing file, retaining comment and include text.
    // Callers can stage a group of edits and new files, then validate the whole graph once.
    struct Edit {
        QString path;
        QStringList keys;
        QVariant value;
    };
    bool apply(const QList<Edit> &edits, const Files &newFiles, QString &error);

  private:
    struct Data;
    std::shared_ptr<const Data> m_data;
};
