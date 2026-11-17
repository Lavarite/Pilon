#include "../../src/calendarmanager.h"
#include "../../../../src/app/keybindings.h"
#include <QApplication>
#include <QImage>
#include <QPointingDevice>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <QtQuickTest/quicktest.h>
#include <memory>
class CalendarUiSetup : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    std::unique_ptr<CalendarManager> manager;
    QPointingDevice touchpad{"Calendar test touchpad", 1, QInputDevice::DeviceType::TouchPad,
        QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position | QInputDevice::Capability::Scroll | QInputDevice::Capability::PixelScroll,
        10, 0};
  public:
    Q_INVOKABLE void touchpadWheel(QQuickItem *item, int pixelDelta, int angleDelta) {
        if (!item || !item->window()) return;
        const QPointF position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QWheelEvent event(position, item->window()->mapToGlobal(position), QPoint(0, pixelDelta),
            QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false,
            Qt::MouseEventSynthesizedBySystem, &touchpad);
        QCoreApplication::sendEvent(item->window(), &event);
    }
    Q_INVOKABLE QVariantMap saveWindow(QQuickItem *item, const QString &path) {
        const QImage image = item && item->window() ? item->window()->grabWindow() : QImage{};
        return {{"saved", !image.isNull() && image.save(path)}, {"width", image.width()},
            {"height", image.height()}, {"devicePixelRatio", image.devicePixelRatio()}};
    }
  public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        manager = std::make_unique<CalendarManager>(directory.path());
        engine->rootContext()->setContextProperty("testCalendar", manager.get());
        engine->rootContext()->setContextProperty("calendarInput", this);
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    qmlRegisterSingletonType<KeyBindings>("Pilon", 1, 0, "KeyBindings", &KeyBindings::create);
    CalendarUiSetup setup;
    return quick_test_main_with_setup(argc, argv, "calendar_ui", nullptr, &setup);
}
#include "calendar_ui_main.moc"
