#pragma once

#include <qpa/qwindowsysteminterface_p.h>
#include <QObject>
#include <QSet>

class TabletInputGuard final : public QObject, public QWindowSystemEventHandler {
public:
    TabletInputGuard();
    bool sendEvent(QWindowSystemInterfacePrivate::WindowSystemEvent *event) override;
private:
    QSet<const QInputDevice *> m_devices;
};
