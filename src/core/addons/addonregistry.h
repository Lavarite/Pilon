#pragma once
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class AddonRegistry : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList addons READ addons NOTIFY changed)
    Q_PROPERTY(QVariantList sidebarAddons READ sidebarAddons NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool recovery READ recovery CONSTANT)
  public:
    explicit AddonRegistry(QObject *parent = nullptr);
    AddonRegistry(const QString &builtins, const QString &installed, const QString &data, bool recovery,
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
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString dataDirectory(const QString &id);
    Q_INVOKABLE bool settingsWritable(const QString &id);
    Q_INVOKABLE QVariant setting(const QString &id, const QString &key, const QVariant &fallback = {});
    Q_INVOKABLE bool setSetting(const QString &id, const QString &key, const QVariant &value);
  signals:
    void changed();

  private:
    AddonRegistry(const QString &builtins, const QString &installed, const QString &data,
                  const QString &state, bool recovery, QObject *parent);
    struct Snapshot {
        QByteArray hash;
        QVariantMap row;
    };
    const QVariantMap *find(const QString &id) const;
    QStringList dependencyOrder(const QString &id, QString *error) const;
    bool configured(const QVariantMap &row) const;
    bool loadState();
    bool saveState();
    bool seedPackages();
    void rearmWatches();
    bool fail(const QString &message);
    QString m_builtins, m_installed, m_data, m_error;
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
