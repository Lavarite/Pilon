#include "tabletinputguard.h"
#include <QInputDevice>
#include <QPointingDevice>
#include <private/qguiapplication_p.h>

TabletInputGuard::TabletInputGuard() {
    QWindowSystemInterfacePrivate::installWindowSystemEventHandler(this);
}

bool TabletInputGuard::sendEvent(QWindowSystemInterfacePrivate::WindowSystemEvent *event) {
    using Events = QWindowSystemInterfacePrivate;
    if (event->type == Events::Tablet || event->type == Events::TabletEnterProximity
        || event->type == Events::TabletLeaveProximity) {
        const auto *tablet = static_cast<const Events::TabletEvent *>(event);
        // Wayland can remove a tablet tool while its events are still queued.
        // Qt 6.12 dereferences that raw device pointer before application filters
        // run. Check the live registry without dereferencing the removed device.
        if (!QInputDevice::devices().contains(tablet->device)) {
            event->eventAccepted = false;
            return false;
        }
        if (!m_devices.contains(tablet->device)) {
            m_devices.insert(tablet->device);
            const auto id = static_cast<const QPointingDevice *>(tablet->device)->uniqueId().numericId();
            connect(tablet->device, &QObject::destroyed, this, [this, device = tablet->device, id] {
                m_devices.remove(device);
                // Its queued release may also be discarded. Clear Qt's held
                // buttons so the next tool's first press remains a press.
                auto &point = QGuiApplicationPrivate::tabletDevicePoint(id);
                point.state = Qt::NoButton;
                point.target = nullptr;
            });
        }
    }
    return QWindowSystemEventHandler::sendEvent(event);
}
