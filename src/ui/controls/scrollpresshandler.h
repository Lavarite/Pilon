#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqmlregistration.h>

class ScrollPressHandler : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(PilonScrollPressHandler)
    Q_PROPERTY(QQuickItem *view READ view WRITE setView NOTIFY viewChanged)
public:
    using QObject::QObject;
    QQuickItem *view() const { return m_view; }
    void setView(QQuickItem *view);

signals:
    void viewChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    void setWindow(QQuickWindow *window);
    QPointer<QQuickItem> m_view;
    QPointer<QQuickWindow> m_window;
};
