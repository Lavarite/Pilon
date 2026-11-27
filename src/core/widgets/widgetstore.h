#pragma once
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class WidgetStore : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString workspaceId READ workspaceId NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool canRequestAdd READ canRequestAdd NOTIFY nativeConfigurationChanged)
public:
    explicit WidgetStore(QObject *parent = nullptr);
    explicit WidgetStore(const QString &directory, QObject *parent = nullptr);
    QString workspaceId() const { return m_workspace; }
    QString error() const { return m_error; }
    bool available() const;
    bool canRequestAdd() const;
    Q_INVOKABLE bool configure(const QString &workspaceRoot, const QVariantList &providers);
    Q_INVOKABLE bool publish(const QString &addonId, const QString &widgetId, const QVariantMap &snapshot);
    Q_INVOKABLE bool clear(const QString &addonId, const QString &widgetId);
    Q_INVOKABLE double generation(const QString &addonId, const QString &widgetId) const;
    Q_INVOKABLE QVariantMap resolve(const QUrl &url, qint64 nowMs = 0);
    Q_INVOKABLE bool requestAdd(const QString &addonId, const QString &widgetId);
    Q_INVOKABLE QVariantList activeProviders() const;
    Q_INVOKABLE void refreshNative();
signals:
    void changed();
    void nativeConfigurationChanged();
private:
    QString key(const QString &addonId, const QString &widgetId) const;
    QString path(const QString &providerKey) const;
    QVariantMap read(const QString &providerKey) const;
    QVariantMap readSnapshot(const QString &providerKey, const QString &owner, const QString &widget) const;
    bool currentCatalog() const;
    bool save(const QString &file, const QVariantMap &value);
    bool ready();
    bool fail(const QString &message);
    QString m_directory, m_workspace, m_error;
    QVariantMap m_providers;
};
