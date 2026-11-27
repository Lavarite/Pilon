#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
class QQmlEngine;
class QJSEngine;
class QPointingDevice;

class KeyBindings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QVariantMap bindings READ bindings NOTIFY changed)
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)
    Q_PROPERTY(QVariantMap stylusBindings READ stylusBindings NOTIFY changed)
    Q_PROPERTY(QVariantList stylusActions READ stylusActions NOTIFY changed)
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
public:
    explicit KeyBindings(QObject *parent = nullptr);
    static KeyBindings *instance();
    void reloadPreferences();
    static KeyBindings *create(QQmlEngine *, QJSEngine *);
    QVariantMap bindings() const;
    QVariantList actions() const;
    QVariantMap stylusBindings() const;
    QVariantList stylusActions() const;
    bool recording() const { return m_recording; }
    void setRecording(bool recording);
    Q_INVOKABLE QString setBinding(const QString &id, const QString &text);
    Q_INVOKABLE QString reset(const QString &id = {});
    Q_INVOKABLE QString keyText(int key, int modifiers) const;
    Q_INVOKABLE bool matches(const QString &id, int key, int modifiers) const;
    Q_INVOKABLE QString setStylusBinding(const QString &button, const QString &tool);
    Q_INVOKABLE QString resetStylusBinding(const QString &button = {});
signals:
    void changed();
    void recordingChanged();
    void inputRecorded(QString input);
protected:
    bool eventFilter(QObject *object, QEvent *event) override;
private:
    struct Action { QString id, label, group; QStringList defaults, contexts; };
    QString conflict(const Action &action, const QStringList &keys) const;
    void clearStylusToolBinding(QVariantMap &bindings, const QString &button) const;
    QString saveBindings(QVariantMap bindings, QVariantMap stylus);
    QList<Action> m_actions;
    QVariantMap m_custom;
    QVariantMap m_stylusCustom;
    bool m_recording = false;
    QPointer<const QPointingDevice> m_recordedStylus;
    Qt::MouseButton m_recordedButton = Qt::NoButton;
};
