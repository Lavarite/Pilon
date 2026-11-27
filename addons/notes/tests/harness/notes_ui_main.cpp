#include "../../../../src/app/keybindings.h"
#include "../../../../src/ui/controls/scrollpresshandler.h"
#include "../../../../src/core/addons/addonregistry.h"
#include "../../../../src/core/widgets/widgetstore.h"
#include "../../../../src/core/workspace/pilonworkspace.h"
#include "../../../../src/core/workspace/pilonsettings.h"
#include <QDir>
#include "../../src/notesmanager.h"
#include "../../src/notecanvas.h"
#include "../../src/notedocument.h"
#include "../../../../src/core/documents/documentpreview.h"
#include "../../../assistant/src/assistantmanager.h"
#include "../../../calendar/src/calendarmanager.h"
#include "../../../files/src/remotefilemanager.h"
#include "../../../ssh/src/sshmanager.h"
#include "../../../devices/src/tailscalemanager.h"
#include "../../../ssh/src/terminalitem.h"
#include "../../../../src/app/tabletinputguard.h"
#include <QtQuickTest/quicktest.h>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QSettings>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QTabletEvent>
#include <QKeyEvent>
#include <QClipboard>
#include <QGuiApplication>
#include <QApplication>
#include <QPalette>
#include <QMimeData>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QAudioBufferOutput>
#include <QAudioBuffer>
#include <cmath>
#include <QPdfWriter>
#include <QPainter>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QWheelEvent>
#include <QtTest/QTest>
#include <QRegularExpression>
#include <qpa/qwindowsysteminterface.h>
#include <qpa/qwindowsysteminterface_p.h>
#include <private/qhighdpiscaling_p.h>
#include <private/qeventpoint_p.h>
#include <private/qinputmethod_p.h>
#include <qpa/qplatforminputcontext.h>
#include <QInputMethodEvent>
#include <QTextCharFormat>
#include <QAccessible>
#include <QSignalSpy>
#include <QCryptographicHash>
#include <memory>

class NotesUiSetup : public QObject {
    Q_OBJECT
    QTemporaryDir m_assets;
    std::unique_ptr<QTest::QTouchEventSequence> m_finger;
    QMap<int, QPoint> m_contacts;
    const QPointingDevice *m_lastTablet = nullptr;
    QAudioBufferOutput *m_audioProbe = nullptr;
    qreal m_audioPeak = 0;
public:
    Q_INVOKABLE QString accessibleName(QObject *target) const {
        auto *interface = QAccessible::queryAccessibleInterface(target);
        return interface ? interface->text(QAccessible::Name) : QString{};
    }
    Q_INVOKABLE void inputMethodSelection(QObject *target, int start, int length) {
        if (!target || start < 0 || length < 0) return;
        QInputMethodEvent event(QString{}, {{QInputMethodEvent::Selection, start, length, QVariant{}}});
        QCoreApplication::sendEvent(target, &event);
    }
    Q_INVOKABLE QObject *modalWindow() const { return QGuiApplication::modalWindow(); }
    Q_INVOKABLE QObject *terminalItem(QObject *root) const { return root ? root->findChild<TerminalItem *>() : nullptr; }
    Q_INVOKABLE QVariantMap terminalImeEvent(QObject *object, const QString &preedit,
        const QString &commit, int replacementStart, int replacementLength, int cursor) {
        auto *terminal = qobject_cast<TerminalItem *>(object);
        if (!terminal) return {};
        QTextCharFormat underline;
        underline.setFontUnderline(true);
        QInputMethodEvent event(preedit, {
            {QInputMethodEvent::TextFormat, 0, int(preedit.size()), underline},
            {QInputMethodEvent::Cursor, cursor, 1, QVariant{}}
        });
        event.setCommitString(commit, replacementStart, replacementLength);
        event.ignore();
        QSignalSpy sent(terminal, &TerminalItem::dataGenerated);
        QCoreApplication::sendEvent(terminal, &event);
        QByteArray bytes;
        for (const auto &entry : sent) bytes += entry.first().toByteArray();
        return {{"accepted", event.isAccepted()}, {"bytes", QString::fromUtf8(bytes)}};
    }
    Q_INVOKABLE QVariantMap terminalImeState(QObject *object) {
        auto *terminal = qobject_cast<TerminalItem *>(object);
        if (!terminal) return {};
        QInputMethodQueryEvent query(Qt::ImQueryAll);
        QCoreApplication::sendEvent(terminal, &query);
        QImage image(terminal->size().toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        terminal->paint(&painter);
        painter.end();
        const auto hash = QCryptographicHash::hash(
            QByteArrayView(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes()),
            QCryptographicHash::Sha256).toHex();
        return {{"image", QString::fromLatin1(hash)},
            {"nativeHintsProperty", terminal->property("inputMethodHints")},
            {"hints", query.value(Qt::ImHints)}, {"enterKey", query.value(Qt::ImEnterKeyType)},
            {"cursor", query.value(Qt::ImCursorPosition)}, {"anchor", query.value(Qt::ImAnchorPosition)},
            {"surrounding", query.value(Qt::ImSurroundingText)},
            {"cursorRectangle", query.value(Qt::ImCursorRectangle)}};
    }
    Q_INVOKABLE QVariantMap terminalInputProbe(QObject *object, int button, const QString &text) {
        auto *terminal = qobject_cast<TerminalItem *>(object);
        if (!terminal || !terminal->window()) return {};
        struct InputContext final : QPlatformInputContext {
            int shows = 0;
            QObject *focused = nullptr;
            void showInputPanel() override { ++shows; focused = QGuiApplication::focusObject(); }
        } context;
        auto *input = QInputMethodPrivate::get(QGuiApplication::inputMethod());
        auto *previous = input->testContext;
        input->testContext = &context;
        const auto restore = qScopeGuard([&] { input->testContext = previous; });
        QSignalSpy sent(terminal, &TerminalItem::dataGenerated);
        const auto point = terminal->mapToScene(QPointF(terminal->width()/2, terminal->height()/2)).toPoint();
        QTest::mouseClick(terminal->window(), static_cast<Qt::MouseButton>(button), Qt::NoModifier, point);
        const bool focused = terminal->hasActiveFocus();
        if (button == Qt::LeftButton && !text.isEmpty()) {
            QInputMethodEvent commit;
            commit.setCommitString(text);
            if (auto *focus = QGuiApplication::focusObject()) QCoreApplication::sendEvent(focus, &commit);
            QTest::keyClick(terminal->window(), Qt::Key_Return);
        }
        QByteArray bytes;
        for (const auto &entry : sent) bytes += entry.first().toByteArray();
        return {{"shows", context.shows}, {"focusAtShow", context.focused == terminal},
            {"focused", focused}, {"bytes", QString::fromUtf8(bytes)}};
    }
    Q_INVOKABLE QUrl settingsUrl() const { return QUrl::fromLocalFile(PilonWorkspace::settingsFile()); }
    Q_INVOKABLE QString settingsPath() const { return PilonWorkspace::settingsFile(); }
    Q_INVOKABLE QString workspacePath() const { return PilonWorkspace::configuredRoot(); }
    Q_INVOKABLE QUrl configurationBundleFixture() const {
        const auto path = QDir(PilonWorkspace::configuredRoot()).filePath("test-settings.piconfpack");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        if (file.write("{}") != 2) return {};
        return QUrl::fromLocalFile(path);
    }
    Q_INVOKABLE QVariant preference(const QString &key) const { return PilonSettings().value(key); }
    Q_INVOKABLE void repeatKey(int key) {
        auto *window = QGuiApplication::focusWindow();
        if (!window) return;
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString{}, true);
        QCoreApplication::sendEvent(window, &release);
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString{}, true);
        QCoreApplication::sendEvent(window, &press);
    }
    Q_INVOKABLE QFont applicationFont() const { return QApplication::font(); }
    Q_INVOKABLE void setApplicationFont(const QFont &font) { QApplication::setFont(font); }
    Q_INVOKABLE bool setDirectoryWritable(const QString &path, bool writable) {
        return QFile::setPermissions(path, QFile::ReadOwner | QFile::ExeOwner
            | (writable ? QFile::WriteOwner : QFile::Permissions{}));
    }
    Q_INVOKABLE QRect screenGeometry() const {
        return QGuiApplication::primaryScreen()->geometry();
    }
    Q_INVOKABLE void setScreenGeometry(const QRect &geometry) {
        auto *screen = QGuiApplication::primaryScreen();
        const auto native = QHighDpi::toNativePixels(geometry, screen);
        QWindowSystemInterface::handleScreenGeometryChange(screen, native, native);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    }
    Q_INVOKABLE void configureWindow(QQuickWindow *window, int width, int height) {
        const auto geometry = QHighDpi::toNativeWindowGeometry(QRect(window->position(), QSize(width, height)), window);
        QWindowSystemInterface::handleGeometryChange(window, geometry);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    }
    Q_INVOKABLE QVariantMap systemPalette() const {
        const auto palette = QApplication::palette();
        return {{"window", palette.color(QPalette::Window)}, {"text", palette.color(QPalette::WindowText)},
            {"highlight", palette.color(QPalette::Highlight)}};
    }
    Q_INVOKABLE void setSystemPalette(const QColor &window, const QColor &text, const QColor &highlight) {
        auto palette = QApplication::palette();
        palette.setColor(QPalette::Window, window); palette.setColor(QPalette::WindowText, text);
        palette.setColor(QPalette::Highlight, highlight);
        QApplication::setPalette(palette);
    }
    Q_INVOKABLE bool observePlayback(QObject *player) {
        delete m_audioProbe; m_audioProbe = new QAudioBufferOutput(this); m_audioPeak = 0;
        connect(m_audioProbe, &QAudioBufferOutput::audioBufferReceived, this, [this](const QAudioBuffer &buffer) {
            if (!buffer.isValid()) return;
            m_audioPeak = 0;
            for (int i = 0; i < buffer.sampleCount(); ++i) {
                double value = 0;
                if (buffer.format().sampleFormat() == QAudioFormat::Int16) value = buffer.constData<qint16>()[i] / 32768.;
                else if (buffer.format().sampleFormat() == QAudioFormat::Float) value = buffer.constData<float>()[i];
                m_audioPeak = std::max(m_audioPeak, std::abs(value));
            }
        });
        return player->setProperty("audioBufferOutput", QVariant::fromValue(m_audioProbe));
    }
    Q_INVOKABLE qreal decodedAudioPeak() const { return m_audioPeak; }
    Q_INVOKABLE void repaintCanvas(NoteCanvas *canvas) { canvas->update(); }
    Q_INVOKABLE bool saveWindow(QQuickItem *item, const QString &path) {
        return item && item->window() && item->window()->grabWindow().save(path);
    }
    Q_INVOKABLE bool removeTabletWithQueuedEvents(QQuickItem *item, bool duringStroke) {
        if (!item || !item->window()) return false;
        auto *pen = new QPointingDevice("removed queued pen", 987123, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure
                | QInputDevice::Capability::Hover, 1, 1);
        QWindowSystemInterface::registerInputDevice(pen);
        const auto logical = item->mapToScene(QPointF(60, 120));
        const auto position = QHighDpi::toNativeLocalPosition(logical, item->window());
        const auto global = QHighDpi::toNativeGlobalPosition(item->window()->mapToGlobal(logical), item->window());
        QWindowSystemInterface::handleTabletEnterLeaveProximityEvent(item->window(), pen, true, position, global);
        if (duringStroke) {
            QWindowSystemInterface::handleTabletEvent(item->window(), pen, position, global, Qt::LeftButton, .7, 0, 0, 0, 0, 0);
            QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        }
        QWindowSystemInterface::handleTabletEvent(item->window(), pen, position + QPointF(30, 10), global + QPointF(30, 10),
            Qt::LeftButton, .7, 0, 0, 0, 0, 0);
        QWindowSystemInterface::handleTabletEvent(item->window(), pen, position, global, Qt::NoButton, 0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::handleTabletEnterLeaveProximityEvent(item->window(), pen, false, position, global);
        delete pen;
        const bool removed = !QInputDevice::devices().contains(pen);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        return removed;
    }
    Q_INVOKABLE QColor pixelAt(QQuickItem *item, qreal x, qreal y) {
        const auto image = item->window()->grabWindow();
        const auto position = item->mapToScene(QPointF(x, y));
        return image.pixelColor(qRound(position.x() * image.width() / item->window()->width()),
            qRound(position.y() * image.height() / item->window()->height()));
    }
    Q_INVOKABLE void expectInputLog(const QString &pattern) {
        QTest::ignoreMessage(QtInfoMsg, QRegularExpression(pattern));
    }
    Q_INVOKABLE void platformTablet(QQuickItem *item, qreal x, qreal y, qreal pressure, int buttons, bool erase = false) {
        static auto pen = new QPointingDevice("Platform pen", 54, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2, {}, {}, QCoreApplication::instance());
        static auto eraser = new QPointingDevice("Platform eraser", 55, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2, {}, {}, QCoreApplication::instance());
        static const bool registered = [] { QWindowSystemInterface::registerInputDevice(pen); QWindowSystemInterface::registerInputDevice(eraser); return true; }();
        Q_UNUSED(registered);
        const auto position = item->mapToScene(QPointF(x, y));
        // Exercise Qt's own tablet-to-mouse fallback for toolbar controls too.
        // Offscreen expects platform mouse events, but does not generate them.
        const bool synthesize = QCoreApplication::testAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents);
        const bool platformSynthesizes = QWindowSystemInterfacePrivate::TabletEvent::platformSynthesizesMouse;
        QWindowSystemInterfacePrivate::TabletEvent::setPlatformSynthesizesMouse(false);
        QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents, true);
        // QPA receives physical pixels; the resulting QTabletEvent is logical.
        const auto local = QHighDpi::toNativeLocalPosition(position, item->window());
        const auto global = QHighDpi::toNativeGlobalPosition(item->window()->mapToGlobal(position), item->window());
        QWindowSystemInterface::handleTabletEvent(item->window(), erase ? eraser : pen, local, global,
            Qt::MouseButtons(buttons), pressure, 0, 0, 0, 0, 0);
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        QCoreApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTabletEvents, synthesize);
        QWindowSystemInterfacePrivate::TabletEvent::setPlatformSynthesizesMouse(platformSynthesizes);
    }
    Q_INVOKABLE void mouseUngrab(QQuickItem *item) {
        QEvent event(QEvent::UngrabMouse); QCoreApplication::sendEvent(item, &event);
    }
    Q_INVOKABLE void tabletMouseCopy(QQuickItem *item, int type, qreal x, qreal y, bool direct = false) {
        const auto position = direct ? QPointF(x, y) : item->mapToScene(QPointF(x, y));
        QMouseEvent event(QEvent::Type(type), position, position, item->window()->mapToGlobal(item->mapToScene(QPointF(x, y))),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::NoModifier, Qt::MouseEventSynthesizedByQt, m_lastTablet);
        QCoreApplication::sendEvent(direct ? static_cast<QObject *>(item) : item->window(), &event);
    }
    Q_INVOKABLE void deviceMouse(QQuickItem *item, int type, qreal x, qreal y, bool touchscreen = false) {
        static QPointingDevice pen("Mouse-only pen", 61, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2);
        static QPointingDevice touch("Touchscreen mouse fallback", 62, QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 1, 0);
        const auto position = item->mapToScene(QPointF(x, y));
        QMouseEvent event(QEvent::Type(type), position, position, item->window()->mapToGlobal(position),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::NoModifier, Qt::MouseEventSynthesizedBySystem, touchscreen ? &touch : &pen);
        QCoreApplication::sendEvent(item->window(), &event);
    }
    Q_INVOKABLE void platformStylusButton(QQuickItem *item, bool pressed, bool tablet = false) {
        // Match the Wayland pen in the physical diagnostic: device "tool",
        // system ID 0, MiddleButton, zero pressure, unsynthesized mouse input.
        static auto pen = new QPointingDevice("tool", 0, QInputDevice::DeviceType::Stylus,
            QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 3);
        static const bool registered = [] { QWindowSystemInterface::registerInputDevice(pen); return true; }();
        Q_UNUSED(registered);
        const auto position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        const auto global = item->window()->mapToGlobal(position);
        const auto buttons = pressed ? Qt::MiddleButton : Qt::NoButton;
        if (tablet) {
            QWindowSystemInterface::handleTabletEvent(item->window(), pen, position, global, buttons, 0, 0, 0, 0, 0, 0);
            QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        } else {
            QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(item->window(), pen,
                position, global, buttons, Qt::MiddleButton, pressed ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
                Qt::NoModifier, Qt::MouseEventNotSynthesized);
        }
    }
    Q_INVOKABLE void removedPen(QQuickItem *item) {
        auto pen = new QPointingDevice("Removed pen", 63, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 2);
        QTabletEvent proximity(QEvent::TabletEnterProximity, pen, {}, {}, 0, 0, 0, 0, 0, 0,
            Qt::NoModifier, Qt::NoButton, Qt::NoButton);
        QCoreApplication::sendEvent(QCoreApplication::instance(), &proximity);
        for (const auto type : {QEvent::TabletPress, QEvent::TabletMove}) {
            const auto position = item->mapToScene(QPointF(type == QEvent::TabletPress ? 100 : 180, 200));
            QTabletEvent event(type, pen, position, item->window()->mapToGlobal(position), .5,
                0, 0, 0, 0, 0, Qt::NoModifier, type == QEvent::TabletPress ? Qt::LeftButton : Qt::NoButton, Qt::LeftButton);
            QCoreApplication::sendEvent(item->window(), &event);
        }
        delete pen;
    }
    Q_INVOKABLE QQuickItem *paperScrollBar(QQuickItem *item) {
        for (auto child : item->parentItem()->findChildren<QQuickItem *>())
            if (child->inherits("QQuickScrollBar") && child->property("orientation").toInt() == Qt::Vertical) return child;
        return nullptr;
    }
    Q_INVOKABLE bool ignoresUnregisteredTouch(QQuickItem *item) {
        QPointingDevice device("Unregistered touchscreen", 48, QInputDevice::DeviceType::TouchScreen,
            QPointingDevice::PointerType::Finger, QInputDevice::Capability::Position, 1, 0);
        QTouchEvent event(QEvent::TouchBegin, &device, Qt::NoModifier,
            {QEventPoint(0, QEventPoint::Pressed, {100, 200}, {100, 200})});
        QCoreApplication::sendEvent(item, &event);
        return !event.isAccepted() && !item->acceptTouchEvents();
    }
    Q_INVOKABLE void eraserMouseStroke(QQuickItem *item) {
        static QPointingDevice eraser("Mouse-delivered eraser", 46, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const auto p = item->mapToScene(QPointF(type == QEvent::MouseButtonPress ? 300 : 120, 200));
            QMouseEvent event(type, p, p, item->window()->mapToGlobal(p),
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier, Qt::MouseEventSynthesizedByQt, &eraser);
            QCoreApplication::sendEvent(item->window(), &event);
        }
    }
    Q_INVOKABLE void tabletEvent(QQuickItem *item, int type, qreal x, qreal y, bool erase = false, int button = Qt::LeftButton, qreal pressure = .5, int buttons = -1) {
        static QPointingDevice pen("Test pen", 44, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        static QPointingDevice eraser("Test eraser", 45, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        m_lastTablet = erase ? &eraser : &pen;
        const auto position = item->mapToScene(QPointF(x, y));
        const auto held = buttons >= 0 ? Qt::MouseButtons(buttons) : type == QEvent::TabletRelease ? Qt::NoButton : Qt::MouseButtons(button);
        QTabletEvent event(QEvent::Type(type), erase ? &eraser : &pen, position, item->window()->mapToGlobal(position),
            type == QEvent::TabletRelease && buttons < 0 ? 0 : pressure, 0, 0, 0, 0, 0, Qt::NoModifier,
            type == QEvent::TabletMove ? Qt::NoButton : Qt::MouseButton(button),
            held);
        event.setAccepted(false);
        QCoreApplication::sendEvent(item->window(), &event);
        // QGuiApplication normally supplies this fallback for unhandled tablet
        // events. sendEvent() bypasses that platform delivery path in this test.
        if (!event.isAccepted()) {
            const auto mouseType = type == QEvent::TabletPress ? QEvent::MouseButtonPress
                : type == QEvent::TabletRelease ? QEvent::MouseButtonRelease : QEvent::MouseMove;
            QMouseEvent mouse(mouseType, position, position, event.globalPosition(), event.button(),
                event.buttons(), event.modifiers(), Qt::MouseEventSynthesizedByQt, event.pointingDevice());
            QCoreApplication::sendEvent(item->window(), &mouse);
        }
    }
    Q_INVOKABLE void fingerDrag(QQuickItem *item, qreal x, qreal y, qreal dx, qreal dy, bool two = false) {
        static auto device = QTest::createTouchDevice();
        const auto p = item->mapToScene(QPointF(x, y)).toPoint(); const auto end = p + QPoint(dx, dy);
        auto touch = QTest::touchEvent(item->window(), device);
        touch.press(0, p, item->window()); if (two) touch.press(1, p + QPoint(30, 30), item->window()); touch.commit();
        touch.move(0, end, item->window()); if (two) touch.stationary(1); touch.commit();
        touch.release(0, end, item->window()); if (two) touch.release(1, p + QPoint(30, 30), item->window()); touch.commit();
    }
    Q_INVOKABLE void penProximity(bool near) {
        QEvent event(near ? QEvent::TabletEnterProximity : QEvent::TabletLeaveProximity);
        QCoreApplication::sendEvent(QCoreApplication::instance(), &event);
    }
    Q_INVOKABLE void fingerEvent(QQuickItem *item, int phase, qreal x, qreal y) {
        contact(item, 0, phase, x, y);
    }
    Q_INVOKABLE void contact(QQuickItem *item, int id, int phase, qreal x, qreal y) {
        static auto device = QTest::createTouchDevice();
        if (phase == 0 && !m_finger) m_finger = std::make_unique<QTest::QTouchEventSequence>(QTest::touchEvent(item->window(), device, false));
        if (!m_finger) return;
        auto &touch = *m_finger;
        const auto position = item->mapToScene(QPointF(x, y)).toPoint();
        for (auto it = m_contacts.cbegin(); it != m_contacts.cend(); ++it) if (it.key() != id) touch.stationary(it.key());
        if (phase == 0) touch.press(id, position, item->window());
        else if (phase == 1) touch.move(id, position, item->window());
        else touch.release(id, position, item->window());
        if (phase == 2) m_contacts.remove(id); else m_contacts[id] = position;
        touch.commit();
        if (m_contacts.isEmpty()) m_finger.reset();
    }
    Q_INVOKABLE bool palmEvent(QQuickItem *item, int phase, qreal x, qreal y, qreal millimeters, int contacts = 1) {
        static auto device = QTest::createTouchDevice();
        const auto *screen = item->window()->screen();
        const qreal diameter = millimeters * screen->physicalDotsPerInch() / screen->devicePixelRatio() / 25.4;
        const auto state = phase == 0 ? QEventPoint::Pressed : phase == 1 ? QEventPoint::Updated : QEventPoint::Released;
        QList<QEventPoint> points;
        for (int i = 0; i < contacts; ++i) {
            const auto position = item->mapToScene(QPointF(x + i * 3, y + i * 3));
            auto point = QMutableEventPoint::withTimeStamp(1000 + phase * 16, i, state, position, position, item->window()->mapToGlobal(position));
            QMutableEventPoint::setDevice(point, device);
            QMutableEventPoint::setEllipseDiameters(point, QSizeF(diameter, diameter));
            points.append(point);
        }
        QTouchEvent event(phase == 0 ? QEvent::TouchBegin : phase == 1 ? QEvent::TouchUpdate : QEvent::TouchEnd,
            device, Qt::NoModifier, points);
        event.setAccepted(false);
        QCoreApplication::sendEvent(item->window(), &event);
        return event.isAccepted();
    }
    Q_INVOKABLE void cancelFinger(QQuickItem *item) {
        QTouchEvent event(QEvent::TouchCancel);
        QCoreApplication::sendEvent(item, &event);
    }
    Q_INVOKABLE bool eraserMovesBeforeRelease(QQuickItem *item) {
        QPointingDevice eraser("Test eraser", 43, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        const auto send = [&](QEvent::Type type, const QPointF &p, qreal pressure, Qt::MouseButtons buttons) {
            const auto position = item->mapToScene(p);
            QTabletEvent event(type, &eraser, position, item->window()->mapToGlobal(position), pressure,
                0, 0, 0, 0, 0, Qt::NoModifier, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton, buttons);
            QCoreApplication::sendEvent(item->window(), &event);
        };
        send(QEvent::TabletMove, {300, 200}, 0, Qt::NoButton);
        send(QEvent::TabletPress, {300, 200}, .5, Qt::LeftButton);
        send(QEvent::TabletMove, {120, 200}, .5, Qt::LeftButton);
        const bool erased = qobject_cast<NoteCanvas *>(item)->strokes().isEmpty();
        send(QEvent::TabletRelease, {120, 200}, 0, Qt::NoButton);
        return erased;
    }
    Q_INVOKABLE QVariantList elementCorners(const QVariantMap &item) {
        QVariantList result;
        for (auto p : NoteDocument::elementTransform(item).map(QPolygonF(QRectF(0, 0, item.value("width").toDouble(), item.value("height").toDouble())))) result.append(p);
        return result;
    }
    Q_INVOKABLE QUrl imageFile() {
        QImage image(160, 100, QImage::Format_RGB32); image.fill(QColor("#007755"));
        const auto path = m_assets.filePath("image.png"); image.save(path); return QUrl::fromLocalFile(path);
    }
    Q_INVOKABLE QUrl unsupportedFile() {
        const auto path = m_assets.filePath("unsupported.bin"); QFile file(path); if (!file.open(QIODevice::WriteOnly)) return {}; file.write("not an image"); return QUrl::fromLocalFile(path);
    }
    Q_INVOKABLE QUrl pdfWithoutSections() {
        const auto path = m_assets.filePath("without-sections.pdf");
        QPdfWriter writer(path);
        QPainter painter(&writer);
        painter.drawText(100, 100, "Document without sections");
        painter.end();
        return QUrl::fromLocalFile(path);
    }
    Q_INVOKABLE int cursorShape(QQuickItem *item) { return int(item->cursor().shape()); }
    Q_INVOKABLE int windowCursor(QQuickItem *item) { return int(item->window()->cursor().shape()); }
    Q_INVOKABLE void completeHandwriting(NotesManager *notes, qint64 revision, const QString &text, int y) {
        notes->findChild<NoteHandwriting*>()->indexed(notes->currentId(), revision,
            {QVariantMap{{"text", text}, {"x", 60}, {"y", y}, {"width", 180}, {"height", 24},
                {"words", QVariantList{QVariantMap{{"start", 0}, {"length", text.size()}, {"x", 60}, {"y", y}, {"width", 180}, {"height", 24}}}}}});
    }
    Q_INVOKABLE void wheel(QQuickItem *item, qreal x, qreal y, int dx, int dy) {
        const auto p = item->mapToScene(QPointF(x, y));
        QWheelEvent event(p, item->window()->mapToGlobal(p), {}, QPoint(dx, dy), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(item->window(), &event);
    }
    Q_INVOKABLE void nativeZoom(QQuickItem *item, qreal x, qreal y, qreal factor) {
        static QPointingDevice device("Test touchpad", 47, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position, 2, 0);
        const auto p = item->mapToScene(QPointF(x, y));
        QNativeGestureEvent event(Qt::ZoomNativeGesture, &device, 2, p, p, item->window()->mapToGlobal(p), factor - 1, {});
        QCoreApplication::sendEvent(item->window(), &event);
    }
    Q_INVOKABLE void pixelScroll(QQuickItem *item, qreal x, qreal y, int dx, int dy, int modifiers = Qt::NoModifier) {
        static QPointingDevice device("Test touchpad scroll", 48, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
            QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 2, 0);
        const auto p = item->mapToScene(QPointF(x, y));
        static ulong timestamp = 100;
        // Include the fallback angle delta and multiple frames supplied by a real touchpad.
        for (const auto phase : {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollUpdate, Qt::ScrollUpdate, Qt::ScrollEnd}) {
            const auto delta = phase == Qt::ScrollUpdate ? QPoint(dx, dy) : QPoint();
            QWheelEvent event(p, item->window()->mapToGlobal(p), delta, delta * 8,
                Qt::NoButton, Qt::KeyboardModifiers(modifiers), phase, false, Qt::MouseEventSynthesizedBySystem, &device);
            event.setTimestamp(timestamp += 16);
            QCoreApplication::sendEvent(item->window(), &event);
        }
    }
    Q_INVOKABLE bool dropFile(QQuickItem *item, const QUrl &url, qreal x, qreal y) {
        const auto p = item->mapToScene(QPointF(x, y)).toPoint(); QMimeData mime; mime.setUrls({url});
        QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &enter);
        QDragMoveEvent move(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &move);
        QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &drop); return drop.isAccepted();
    }
    Q_INVOKABLE bool dropEntries(QQuickItem *item, const QVariantList &entries, qreal x, qreal y) {
        const auto p = item->mapToScene(QPointF(x, y)).toPoint(); QMimeData mime;
        mime.setData("application/x-pilon-remote-entries", QJsonDocument(QJsonArray::fromVariantList(entries)).toJson());
        QDragEnterEvent enter(p, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &enter);
        QDragMoveEvent move(p, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &move);
        QDropEvent drop(p, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier); QCoreApplication::sendEvent(item->window(), &drop); return drop.isAccepted();
    }
    Q_INVOKABLE void clipboardImage() {
        QImage image(160, 100, QImage::Format_RGB32); image.fill(QColor("#007755"));
        QGuiApplication::clipboard()->setImage(image);
    }
    Q_INVOKABLE void clearClipboard() { QGuiApplication::clipboard()->clear(); }
    Q_INVOKABLE QString clipboardText() { return QGuiApplication::clipboard()->text(); }
    Q_INVOKABLE void tabletStroke(QQuickItem *item) {
        QPointingDevice pen("Test tablet", 42, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
        const auto send = [&](QEvent::Type type, const QPointF &p, qreal pressure) {
            const auto position = item->mapToScene(p);
            QTabletEvent event(type, &pen, position, item->window()->mapToGlobal(position), pressure,
                0, 0, 0, 0, 0, Qt::NoModifier, type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton);
            QCoreApplication::sendEvent(item->window(), &event);
        };
        send(QEvent::TabletPress, {60, 200}, .25);
        send(QEvent::TabletMove, {140, 210}, .65);
        send(QEvent::TabletRelease, {220, 220}, .9);
    }
public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty("tabletInput", this);
        engine->rootContext()->setContextProperty("exampleAddonPackage", QUrl::fromLocalFile(PILON_EXAMPLE_PACKAGE));
        engine->rootContext()->setContextProperty("widgetTestPackage", QUrl::fromLocalFile(PILON_WIDGET_TEST_PACKAGE));
        engine->rootContext()->setContextProperty("checklistAddonPackage", QUrl::fromLocalFile(PILON_CHECKLIST_PACKAGE));
        engine->rootContext()->setContextProperty("dependencyProviderAddonPackage", QUrl::fromLocalFile(PILON_DEPENDENCY_PROVIDER_PACKAGE));
        engine->rootContext()->setContextProperty("dependencyConsumerAddonPackage", QUrl::fromLocalFile(PILON_DEPENDENCY_CONSUMER_PACKAGE));
    }
    void applicationAvailable() {
        QCoreApplication::setOrganizationName("PilonTests");
        QCoreApplication::setApplicationName("NotesUI");
        QSettings().setValue("notes/directory", m_assets.filePath("notes"));
        QSettings().setValue("calendar/directory", m_assets.filePath("calendar"));
        QSettings().setValue("workspace/root", m_assets.filePath("workspace"));
        QDir().mkpath(m_assets.filePath("workspace/.pilon"));
        QSettings bootstrap(PilonWorkspace::bootstrapFile(), QSettings::IniFormat);
        bootstrap.setValue("workspace/version", 1);
        bootstrap.setValue("workspace/root", m_assets.filePath("workspace")); bootstrap.sync();
        QSettings active(PilonWorkspace::settingsFile(), QSettings::IniFormat);
        active.setValue("notes/directory", m_assets.filePath("notes"));
        active.setValue("calendar/directory", m_assets.filePath("calendar")); active.sync();
        qputenv("PILON_MODELS_DIR", m_assets.filePath("models").toUtf8());
        if (!qEnvironmentVariableIsSet("PILON_TEST_NO_TOUCH")) QTest::createTouchDevice();
        qmlRegisterTypesAndRevisions<KeyBindings>("Pilon", 1);
        qmlRegisterTypesAndRevisions<ScrollPressHandler>("Pilon", 1);
        qmlRegisterType<AddonRegistry>("Pilon", 1, 0, "AddonRegistry");
        qmlRegisterType<WidgetStore>("Pilon", 1, 0, "WidgetStore");
        for (const auto &folder : {QString(PILON_QML_SOURCE_DIR), QString(PILON_QML_SOURCE_DIR) + "/ui"}) {
            for (const auto &file : QDir(folder).entryList({"*.qml"}, QDir::Files)) {
                const auto name = file.chopped(4).toUtf8();
                qmlRegisterType(QUrl::fromLocalFile(folder + '/' + file), "Pilon", 1, 0, name.constData());
            }
        }
        qmlRegisterType<NotesManager>("Pilon", 1, 0, "NotesManager");
        qmlRegisterType<NoteCanvas>("Pilon", 1, 0, "NoteCanvas");
        qmlRegisterType<NoteTextFormat>("Pilon", 1, 0, "NoteTextFormat");
        qmlRegisterType<NoteTextLayout>("Pilon", 1, 0, "NoteTextLayout");
        qmlRegisterType<DocumentPreview>("Pilon", 1, 0, "DocumentPreview");
        qmlRegisterType<AssistantManager>("Pilon", 1, 0, "AssistantManager");
        qmlRegisterType<CalendarManager>("Pilon", 1, 0, "CalendarManager");
        qmlRegisterType<RemoteFileManager>("Pilon", 1, 0, "RemoteFileManager");
        qmlRegisterType<SshManager>("Pilon", 1, 0, "SshManager");
        qmlRegisterType<TailscaleManager>("Pilon", 1, 0, "TailscaleManager");
        qmlRegisterType<TerminalItem>("Pilon", 1, 0, "SshTerminal");
    }
};
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    // Qt Quick also coalesces touch updates between frames; ink and lassos need every sample.
    qputenv("QML_NO_TOUCH_COMPRESSION", "1");
    QTemporaryDir data;
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", data.path().toUtf8());
    QApplication app(argc, argv);
    TabletInputGuard tabletInputGuard;
    NotesUiSetup setup;
    return quick_test_main_with_setup(argc, argv, "Notes", nullptr, &setup);
}
#include "notes_ui_main.moc"
