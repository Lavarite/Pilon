#pragma once

#include "piconfdocument.h"
#include <QFileSystemWatcher>
#include <QTimer>
#include <functional>

class QSettings;

class PiconfPreferences : public QObject {
  public:
    static PiconfPreferences &instance();
    bool start(const QString &workspace, QString &error);
    void stop();
    // Qt INI files discard numeric and boolean scalar types on a new process.
    static QVariant scopedValue(const QSettings &store, const QString &key, const QVariant &fallback = {});
    static void setScopedType(QSettings &store, const QString &key, const QVariant &value);
    static QVariantMap supportedAddons(QVariantMap declarations);
    bool flush(QString &error);
    bool prepareInstalled(const QString &id, QString &error);
    bool validate(const PiconfDocument &document, QString &error) const;
    bool prepareImported(const PiconfDocument &document, PiconfDocument::Files &expected, PiconfDocument::Files &updated,
                         QString &error) const;
    bool active() const { return m_active; }
    std::function<void(const QString &)> reportError;

  private:
    PiconfPreferences();
    bool reconcile(bool starting, QString &error);
    void watch(const QStringList &paths);
    QString m_root;
    bool m_active = false;
    QFileSystemWatcher m_watcher;
    QTimer m_timer;
};
