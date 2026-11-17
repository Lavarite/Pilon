#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QTabletEvent>
#include <QTextStream>
#include <QTimer>

// Manual diagnostic for the same Qt Quick input path used by Notes.
// Observe events without consuming tablet events or disabling mouse fallback.
class InputLog : public QObject {
public:
    explicit InputLog(bool verbose) : m_verbose(verbose) {}

    bool eventFilter(QObject *receiver, QEvent *event) override {
        QJsonObject entry{{"layer", "Qt"}, {"receiver", QString::fromLatin1(receiver->metaObject()->className())},
            {"receiverName", receiver->objectName()}, {"window", receiver->isWindowType()}};
        const QPointingDevice *device = nullptr;
        Qt::MouseButton button = Qt::NoButton;
        Qt::MouseButtons buttons;
        qreal pressure = 0;
        bool motion = false;
        QString family;
        switch (event->type()) {
        case QEvent::TabletPress:
        case QEvent::TabletRelease:
        case QEvent::TabletMove:
        case QEvent::TabletEnterProximity:
        case QEvent::TabletLeaveProximity: {
            const auto tablet = static_cast<QTabletEvent *>(event);
            device = tablet->pointingDevice(); button = tablet->button(); buttons = tablet->buttons();
            pressure = tablet->pressure(); family = "tablet";
            motion = event->type() == QEvent::TabletMove;
            entry["type"] = event->type() == QEvent::TabletPress ? "TabletPress"
                : event->type() == QEvent::TabletRelease ? "TabletRelease"
                : motion ? "TabletMove" : event->type() == QEvent::TabletEnterProximity ? "TabletEnterProximity" : "TabletLeaveProximity";
            break;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove: {
            const auto mouse = static_cast<QMouseEvent *>(event);
            device = mouse->pointingDevice(); button = mouse->button(); buttons = mouse->buttons();
            pressure = mouse->points().isEmpty() ? 0 : mouse->points().first().pressure(); family = "mouse";
            motion = event->type() == QEvent::MouseMove;
            entry["type"] = event->type() == QEvent::MouseButtonPress ? "MouseButtonPress"
                : event->type() == QEvent::MouseButtonRelease ? "MouseButtonRelease"
                : motion ? "MouseMove" : "MouseButtonDblClick";
            entry["source"] = int(mouse->source());
            break;
        }
        case QEvent::KeyPress:
        case QEvent::KeyRelease: {
            const auto key = static_cast<QKeyEvent *>(event);
            entry["type"] = event->type() == QEvent::KeyPress ? "KeyPress" : "KeyRelease";
            entry["key"] = key->key(); entry["nativeScanCode"] = int(key->nativeScanCode());
            entry["nativeVirtualKey"] = int(key->nativeVirtualKey()); entry["modifiers"] = int(key->modifiers());
            entry["autoRepeat"] = key->isAutoRepeat();
            write(entry);
            return false;
        }
        default: return false;
        }
        if (device) {
            entry["device"] = device->name();
            entry["deviceType"] = QString::fromLatin1(QMetaEnum::fromType<QInputDevice::DeviceTypes>().valueToKey(int(device->type())));
            entry["pointerType"] = QString::fromLatin1(QMetaEnum::fromType<QPointingDevice::PointerTypes>().valueToKey(int(device->pointerType())));
            entry["deviceId"] = QString::number(device->systemId());
        }
        entry["button"] = int(button); entry["buttons"] = int(buttons);
        entry["buttonNames"] = buttonNames(buttons); entry["pressure"] = pressure;
        entry["hasPressure"] = pressure > 0;
        const auto stateKey = QString::number(quintptr(receiver)) + '/' + QString::number(quintptr(device)) + '/' + family;
        const auto state = QString::number(int(buttons)) + '/' + QString::number(pressure > 0);
        if (m_verbose || !motion || m_states.value(stateKey) != state) write(entry);
        m_states[stateKey] = state;
        return false;
    }

    static void write(const QJsonObject &entry) {
        QTextStream stream(stdout);
        stream << QJsonDocument(entry).toJson(QJsonDocument::Compact) << '\n';
        stream.flush();
    }

private:
    static QString buttonNames(Qt::MouseButtons buttons) {
        QStringList names;
        const auto meta = QMetaEnum::fromType<Qt::MouseButtons>();
        for (unsigned bit = 1; bit <= unsigned(Qt::MaxMouseButton); bit <<= 1) {
            if (!(unsigned(buttons) & bit)) continue;
            const char *name = meta.valueToKey(int(bit));
            names.append(name ? QString::fromLatin1(name) : QString::number(bit));
        }
        return names.join(',');
    }
    bool m_verbose;
    QHash<QString, QString> m_states;
};

class InputSurface : public QQuickPaintedItem {
public:
    explicit InputSurface(QQuickItem *parent) : QQuickPaintedItem(parent) {
        setObjectName("stylus-test-surface");
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setFlag(ItemIsFocusScope);
        setFocus(true);
    }
    void paint(QPainter *painter) override {
        painter->fillRect(boundingRect(), Qt::white);
        painter->setPen(Qt::black);
        painter->drawText(boundingRect().adjusted(16, 16, -16, -16), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
            "Hover the pen over this window. Press and release each side button.\n\n"
            "Repeat with the tip touching the surface, then lift the tip.\n\n"
            "Try the top button too, if your pen has one. Keep this window focused.\n\n"
            "Events appear in the terminal and the log file. Close this window to finish.");
    }
protected:
    void mousePressEvent(QMouseEvent *event) override { forceActiveFocus(); event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
};

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const auto arguments = app.arguments();
    InputLog logger(arguments.contains("--verbose"));
    app.installEventFilter(&logger);
    InputLog::write({{"layer", "Qt"}, {"type", "environment"}, {"version", qVersion()},
        {"platform", QGuiApplication::platformName()}});
    QQuickWindow window;
    window.setObjectName("stylus-test-window");
    window.setTitle("Pilon stylus button test");
    window.resize(640, 360);
    auto surface = new InputSurface(window.contentItem());
    surface->setSize(window.size());
    QObject::connect(&window, &QQuickWindow::widthChanged, surface, [&] { surface->setWidth(window.width()); });
    QObject::connect(&window, &QQuickWindow::heightChanged, surface, [&] { surface->setHeight(window.height()); });
    const auto durationIndex = arguments.indexOf("--duration");
    if (durationIndex >= 0 && durationIndex + 1 < arguments.size())
        QTimer::singleShot(qRound(arguments[durationIndex + 1].toDouble() * 1000), &app, &QCoreApplication::quit);
    window.show();
    window.requestActivate();
    return app.exec();
}
