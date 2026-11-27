#pragma once
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class QNetworkReply;

class AddonRegistry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList addons READ addons NOTIFY changed)
    Q_PROPERTY(QVariantList sidebarAddons READ sidebarAddons NOTIFY sidebarChanged)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString restoringAddon READ restoringAddon NOTIFY restoreChanged)
    Q_PROPERTY(bool recovery READ recovery CONSTANT)
  public:
    explicit AddonRegistry(QObject *parent = nullptr);
    AddonRegistry(const QString &builtins, const QString &installed, const QString &state, bool recovery,
                  QObject *parent = nullptr);
    QVariantList addons() const;
    QVariantList sidebarAddons() const;
    QString error() const {
        return m_error;
    }
    bool recovery() const {
        return m_recovery;
    }
    Q_INVOKABLE bool isEnabled(const QString &id) const;
    Q_INVOKABLE bool setEnabled(const QString &id, bool enabled);
    Q_INVOKABLE bool disableWithDependents(const QString &id, const QStringList &confirmedDependents);
    Q_INVOKABLE bool setSidebarOrder(const QStringList &ids);
    Q_INVOKABLE bool setPageVisible(const QString &id, bool visible);
    Q_INVOKABLE QStringList requiredOrder(const QString &id);
    Q_INVOKABLE QStringList dependents(const QString &id) const;
    Q_INVOKABLE QVariantList integrationRecords(const QString &target = {}) const;
    Q_INVOKABLE bool setIntegrationEnabled(const QString &owner, const QString &id, bool enabled);
    Q_INVOKABLE QVariantList lifecycleOwners() const;
    Q_INVOKABLE bool finalizeRemoval(const QString &id, bool cleanupSucceeded = false,
                                     const QVariantMap &ownedPaths = {});
    Q_INVOKABLE bool install(const QUrl &package);
    Q_INVOKABLE bool installConfigured(const QUrl &package, const QString &id, const QString &version, const QString &sha256);
    Q_INVOKABLE bool installConfiguredBuiltin(const QString &id, const QString &version, const QString &sha256);
    Q_INVOKABLE bool hasBundledPackage(const QString &id) const;
    Q_INVOKABLE bool downloadConfigured(const QUrl &source, const QString &id, const QString &version, const QString &sha256);
    Q_INVOKABLE void cancelConfiguredDownload();
    QString restoringAddon() const { return m_restoring; }
    Q_INVOKABLE bool refresh();
    Q_INVOKABLE QString dataDirectory(const QString &id);
    Q_INVOKABLE bool settingsWritable(const QString &id);
    Q_INVOKABLE QVariant setting(const QString &id, const QString &key, const QVariant &fallback = {});
    Q_INVOKABLE bool setSetting(const QString &id, const QString &key, const QVariant &value);
    static constexpr qint64 MaxPackageBytes = 128 * 1024 * 1024;
  signals:
    void sidebarChanged();
    void changed();
    void restoreChanged();
    void configuredDownloadFinished(const QString &id, bool installed, const QString &error);

  private:
    struct Snapshot {
        QByteArray hash;
        QVariantMap row;
    };
    const QVariantMap *find(const QString &id) const;
    QStringList dependencyOrder(const QString &id, QString *error) const;
    bool changeEnabled(const QString &id, bool enabled, const QStringList &confirmedDependents);
    bool configured(const QVariantMap &row) const;
    bool loadState();
    bool saveState();
    bool seedPackages();
    bool migrateFlatPackages();
    QString packagePath(const QString &id) const;
    QStringList packagePaths() const;
    bool installPackage(const QUrl &package, const QString &id, const QString &version, const QString &sha256);
    QString m_restoring;
    QPointer<QNetworkReply> m_download;
    void rearmWatches();
    bool fail(const QString &message);
    QString m_builtins, m_installed, m_error;
    bool m_recovery = false;
    QList<QVariantMap> m_rows;
    QHash<QString, Snapshot> m_snapshots;
    QString m_stateDirectory;
    QStringList m_seeded;
    QVariantMap m_seedHashes;
    QVariantMap m_receipts;
    bool m_stateLoaded = false;
    QFileSystemWatcher m_watcher;
    QTimer m_refreshTimer;
};
