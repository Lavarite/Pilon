#include "keybindings.h"
#include <QCoreApplication>
#include <QKeySequence>
#include <QQmlEngine>
#include "../core/workspace/pilonsettings.h"
#include <QMouseEvent>
#include <QTabletEvent>
#include <QDebug>

namespace {
QString stylusInput(const QString &button) {
    return button == "right" ? QStringLiteral("Stylus primary button") : QStringLiteral("Stylus secondary button");
}
bool isStylusInput(const QString &input) {
    return input == stylusInput("right") || input == stylusInput("middle");
}
QString stylusTool(const QString &action) {
    if (action == "noteToolPointer" || action == "noteToolPointerHold") return "pointer";
    if (action == "noteToolPen") return "pen";
    if (action == "noteToolEraser") return "eraser";
    if (action == "noteToolPan") return "pan";
    if (action == "noteToolText") return "text";
    return {};
}
}

KeyBindings::KeyBindings(QObject *parent) : QObject(parent) {
    const QStringList workspace{"files", "notes", "viewer"};
    const auto add = [&](QString id, QString label, QString group, QStringList keys, QStringList contexts) {
        m_actions.append({id, label, group, keys, contexts});
    };
    add("copy", tr("Copy"), tr("Editing"), {"Ctrl+C", "Ctrl+Ins"}, workspace);
    add("cut", tr("Cut"), tr("Editing"), {"Ctrl+X", "Shift+Del"}, {"files", "notes"});
    add("paste", tr("Paste"), tr("Editing"), {"Ctrl+V", "Shift+Ins"}, {"files", "notes"});
    add("selectAll", tr("Select all"), tr("Editing"), {"Ctrl+A"}, workspace);
    add("undo", tr("Undo"), tr("Editing"), {"Ctrl+Z"}, {"files", "notes"});
    add("redo", tr("Redo"), tr("Editing"), {"Ctrl+Shift+Z", "Ctrl+Y"}, {"notes"});
    add("delete", tr("Delete"), tr("Editing"), {"Del"}, {"files", "notes"});
    add("rename", tr("Rename"), tr("Editing"), {"F2"}, {"files", "notes"});
    add("cancel", tr("Cancel / close"), tr("Navigation"), {"Esc"}, {"files", "notes", "viewer", "picker", "models"});
    add("open", tr("Open selection"), tr("Navigation"), {"Return"}, {"files", "notes"});
    add("parent", tr("Parent folder"), tr("Navigation"), {"Backspace", "Alt+Up"}, {"files", "notes"});
    add("location", tr("Edit location"), tr("Navigation"), {"Ctrl+L"}, {"files", "notes"});
    add("newFolder", tr("New folder"), tr("Navigation"), {"Ctrl+Shift+N"}, {"files", "notes"});
    add("favorite", tr("Toggle favorite"), tr("Notes"), {"Ctrl+D"}, {"notes"});
    add("newNote", tr("New note"), tr("Notes"), {"Ctrl+N"}, {"notes"});
    add("save", tr("Save note"), tr("Notes"), {"Ctrl+S"}, {"notes"});
    add("noteToolPen", tr("Pen tool"), tr("Notes tools"), {"B"}, {"notes"});
    add("noteToolPointer", tr("Select and move tool"), tr("Notes tools"), {"V"}, {"notes"});
    add("noteToolPointerHold", tr("Hold select and move tool"), tr("Notes tools"), {}, {"notes"});
    add("noteToolEraser", tr("Eraser tool"), tr("Notes tools"), {"E"}, {"notes"});
    add("noteToolPan", tr("Pan tool"), tr("Notes tools"), {"H"}, {"notes"});
    add("noteToolText", tr("Place text tool"), tr("Notes tools"), {"T"}, {"notes"});
    add("bold", tr("Bold"), tr("Notes"), {"Ctrl+B"}, {"notes"});
    add("italic", tr("Italic"), tr("Notes"), {"Ctrl+I"}, {"notes"});
    add("underline", tr("Underline"), tr("Notes"), {"Ctrl+U"}, {"notes"});
    add("strikeout", tr("Strikethrough"), tr("Notes"), {"Ctrl+Shift+X"}, {"notes"});
    add("largerText", tr("Increase text size"), tr("Notes"), {"Ctrl+Shift+>"}, {"notes"});
    add("smallerText", tr("Decrease text size"), tr("Notes"), {"Ctrl+Shift+<"}, {"notes"});
    add("zoomIn", tr("Zoom in"), tr("View"), {"Ctrl++", "Ctrl+="}, {"files", "viewer", "terminal"});
    add("zoomOut", tr("Zoom out"), tr("View"), {"Ctrl+-"}, {"files", "viewer", "terminal"});
    add("search", tr("Search"), tr("View"), {"Ctrl+F"}, workspace);
    add("pageDown", tr("Page down"), tr("File viewer"), {"PgDown"}, {"viewer"});
    add("pageUp", tr("Page up"), tr("File viewer"), {"PgUp"}, {"viewer"});
    add("previousFile", tr("Previous file"), tr("File viewer"), {"Left", "Alt+Left"}, {"viewer"});
    add("nextFile", tr("Next file"), tr("File viewer"), {"Right", "Alt+Right"}, {"viewer"});
    add("scrollUp", tr("Scroll up"), tr("File viewer"), {"Up"}, {"viewer"});
    add("scrollDown", tr("Scroll down"), tr("File viewer"), {"Down"}, {"viewer"});
    add("findNext", tr("Next match"), tr("File viewer"), {"F3"}, {"viewer"});
    add("findPrevious", tr("Previous match"), tr("File viewer"), {"Shift+F3"}, {"viewer"});
    add("sendMessage", tr("Send message"), tr("Chat"), {"Return"}, {"chat"});
    add("terminalCopy", tr("Copy"), tr("Terminal"), {"Ctrl+Shift+C", "Ctrl+Ins"}, {"terminal"});
    add("terminalPaste", tr("Paste"), tr("Terminal"), {"Ctrl+Shift+V", "Shift+Ins"}, {"terminal"});
    add("terminalDeleteWord", tr("Delete previous word"), tr("Terminal"), {"Ctrl+Backspace"}, {"terminal"});
    add("activateModel", tr("Select model"), tr("Models"), {"Return", "Space"}, {"models"});
    add("modelMenu", tr("Model menu"), tr("Models"), {"Menu", "Shift+F10"}, {"models"});
    m_custom = PilonSettings().value("keybindings/overrides").toMap();
    m_stylusCustom = PilonSettings().value("keybindings/stylus").toMap();
    if (qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) QCoreApplication::instance()->installEventFilter(this);
}
KeyBindings *KeyBindings::instance() { static auto value = new KeyBindings(QCoreApplication::instance()); return value; }
KeyBindings *KeyBindings::create(QQmlEngine *, QJSEngine *) { auto value = instance(); QQmlEngine::setObjectOwnership(value, QQmlEngine::CppOwnership); return value; }
void KeyBindings::setRecording(bool recording) {
    if (m_recording == recording) return;
    m_recording = recording;
    if (recording) QCoreApplication::instance()->installEventFilter(this);
    else if (!m_recordedStylus && !qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) QCoreApplication::instance()->removeEventFilter(this);
    if (qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) qInfo() << "[keybindings.input] recording" << recording;
    emit recordingChanged();
}
bool KeyBindings::eventFilter(QObject *object, QEvent *event) {
    if (qEnvironmentVariableIsSet("PILON_INPUT_DEBUG") && (event->type() == QEvent::TabletPress
        || event->type() == QEvent::TabletRelease || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease)) {
        const auto pointer = static_cast<QSinglePointEvent *>(event);
        qInfo() << "[keybindings.input]" << event->type() << "recording" << m_recording
            << "receiver" << object->metaObject()->className() << object->objectName() << "window" << object->isWindowType()
            << "button" << pointer->button() << "buttons" << pointer->buttons()
            << "device" << pointer->pointingDevice()->name() << pointer->deviceType() << pointer->pointerType();
    }
    if (!m_recording && !m_recordedStylus) {
        m_recordedButton = Qt::NoButton;
        if (!qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) QCoreApplication::instance()->removeEventFilter(this);
        return false;
    }
    if (event->type() == QEvent::TabletLeaveProximity) {
        m_recordedStylus.clear(); m_recordedButton = Qt::NoButton;
        if (!m_recording && !qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) QCoreApplication::instance()->removeEventFilter(this);
        return false;
    }
    if (!object->isWindowType()) return false;
    const QPointingDevice *device = nullptr;
    Qt::MouseButtons buttons;
    if (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove || event->type() == QEvent::TabletRelease) {
        const auto tablet = static_cast<QTabletEvent *>(event);
        device = tablet->pointingDevice(); buttons = tablet->buttons();
    } else if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease) {
        const auto mouse = static_cast<QMouseEvent *>(event);
        if (mouse->deviceType() != QInputDevice::DeviceType::Stylus
            && mouse->pointerType() != QPointingDevice::PointerType::Pen && mouse->pointerType() != QPointingDevice::PointerType::Eraser) return false;
        device = mouse->pointingDevice(); buttons = mouse->buttons();
    } else return false;
    if (m_recordedStylus == device && m_recordedButton != Qt::NoButton) {
        // Keep Qt's mouse fallback from opening a menu or triggering the newly
        // recorded binding before the button used to record it has been lifted.
        if (!(buttons & m_recordedButton)) {
            m_recordedStylus.clear(); m_recordedButton = Qt::NoButton;
            if (!m_recording && !qEnvironmentVariableIsSet("PILON_INPUT_DEBUG")) QCoreApplication::instance()->removeEventFilter(this);
        }
        event->accept(); return true;
    }
    if (!m_recording) return false;
    const auto button = buttons & Qt::RightButton ? Qt::RightButton : buttons & Qt::MiddleButton ? Qt::MiddleButton : Qt::NoButton;
    if (button == Qt::NoButton) return false;
    m_recordedStylus = device; m_recordedButton = button;
    emit inputRecorded(stylusInput(button == Qt::RightButton ? "right" : "middle"));
    event->accept(); return true;
}
QVariantMap KeyBindings::bindings() const {
    QVariantMap result;
    for (const auto &action : m_actions) result[action.id] = m_custom.value(action.id, action.defaults);
    return result;
}
QVariantList KeyBindings::actions() const {
    QVariantList result; const auto keys = bindings();
    for (const auto &action : m_actions) result.append(QVariantMap{{"id", action.id}, {"label", action.label}, {"group", action.group},
        {"keys", keys.value(action.id).toStringList().join("; ")}, {"custom", m_custom.contains(action.id)}});
    return result;
}
QVariantMap KeyBindings::stylusBindings() const {
    QVariantMap result{{"right", m_stylusCustom.value("right", "pointer")},
        {"middle", m_stylusCustom.value("middle", "none")}};
    for (const auto &action : m_actions) {
        const auto tool = stylusTool(action.id);
        if (tool.isEmpty()) continue;
        const auto keys = m_custom.value(action.id, action.defaults).toStringList();
        for (const auto &button : {QStringLiteral("right"), QStringLiteral("middle")})
            if (keys.contains(stylusInput(button))) result[button] = tool;
    }
    return result;
}
QVariantList KeyBindings::stylusActions() const {
    const auto all = stylusBindings();
    const auto custom = [&](const QString &button) {
        return m_stylusCustom.contains(button) || std::any_of(m_actions.cbegin(), m_actions.cend(), [&](const Action &action) {
            return !stylusTool(action.id).isEmpty() && m_custom.value(action.id, action.defaults).toStringList().contains(stylusInput(button));
        });
    };
    return {QVariantMap{{"id", "right"}, {"label", tr("Primary stylus button")}, {"tool", all.value("right")}, {"custom", custom("right")}},
        QVariantMap{{"id", "middle"}, {"label", tr("Secondary stylus button")}, {"tool", all.value("middle")}, {"custom", custom("middle")}}};
}
void KeyBindings::clearStylusToolBinding(const QString &button) {
    for (const auto &action : m_actions) {
        if (stylusTool(action.id).isEmpty()) continue;
        auto keys = m_custom.value(action.id, action.defaults).toStringList();
        if (keys.removeAll(stylusInput(button))) m_custom[action.id] = keys;
    }
    PilonSettings().setValue("keybindings/overrides", m_custom);
}
QString KeyBindings::setStylusBinding(const QString &button, const QString &tool) {
    if (button != "right" && button != "middle") return tr("Unknown stylus button.");
    if (tool != "none" && tool != "pointer" && tool != "pen" && tool != "eraser" && tool != "pan" && tool != "text")
        return tr("Unknown Notes tool.");
    clearStylusToolBinding(button);
    m_stylusCustom[button] = tool;
    PilonSettings().setValue("keybindings/stylus", m_stylusCustom);
    emit changed();
    return {};
}
QString KeyBindings::resetStylusBinding(const QString &button) {
    if (button.isEmpty()) { m_stylusCustom.clear(); clearStylusToolBinding("right"); clearStylusToolBinding("middle"); }
    else {
        if (button != "right" && button != "middle") return tr("Unknown stylus button.");
        clearStylusToolBinding(button);
        m_stylusCustom.remove(button);
    }
    PilonSettings().setValue("keybindings/stylus", m_stylusCustom);
    emit changed();
    return {};
}
QString KeyBindings::setBinding(const QString &id, const QString &text) {
    const auto current = std::find_if(m_actions.cbegin(), m_actions.cend(), [&](const Action &action) { return action.id == id; });
    if (current == m_actions.cend()) return tr("Unknown action.");
    QStringList keys;
    for (const auto &part : text.split(';', Qt::SkipEmptyParts)) {
        if (isStylusInput(part.trimmed())) {
            if (stylusTool(id).isEmpty()) return tr("Stylus buttons can be assigned to Notes tools.");
            if (!keys.contains(part.trimmed())) keys.append(part.trimmed());
            continue;
        }
        const auto key = QKeySequence::fromString(part.trimmed(), QKeySequence::PortableText);
        if (key.isEmpty() || key.count() != 1 || key[0].key() == Qt::Key_unknown) return tr("Enter a key combination, such as Ctrl+N.");
        const auto canonical = key.toString(QKeySequence::PortableText);
        if (!keys.contains(canonical)) keys.append(canonical);
    }
    const auto error = conflict(*current, keys);
    if (!error.isEmpty()) return error;
    m_custom[id] = keys; PilonSettings().setValue("keybindings/overrides", m_custom); emit changed(); return {};
}
QString KeyBindings::conflict(const Action &current, const QStringList &keys) const {
    const auto all = bindings();
    for (const auto &action : m_actions) {
        if (action.id == current.id || !std::any_of(action.contexts.cbegin(), action.contexts.cend(), [&](const QString &context) { return current.contexts.contains(context); })) continue;
        for (const auto &key : all.value(action.id).toStringList()) if (keys.contains(isStylusInput(key) ? key : QKeySequence(key).toString(QKeySequence::PortableText)))
            return tr("Already used by %1.").arg(action.label);
    }
    return {};
}
QString KeyBindings::reset(const QString &id) {
    if (id.isEmpty()) { m_custom.clear(); m_stylusCustom.clear(); PilonSettings().remove("keybindings/stylus"); }
    else {
        const auto action = std::find_if(m_actions.cbegin(), m_actions.cend(), [&](const Action &action) { return action.id == id; });
        if (action == m_actions.cend()) return tr("Unknown action.");
        const auto error = conflict(*action, action->defaults);
        if (!error.isEmpty()) return error;
        m_custom.remove(id);
    }
    PilonSettings().setValue("keybindings/overrides", m_custom); emit changed(); return {};
}
QString KeyBindings::keyText(int key, int modifiers) const {
    if (key == Qt::Key_unknown || key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) return {};
    return QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers), Qt::Key(key))).toString(QKeySequence::PortableText);
}
bool KeyBindings::matches(const QString &id, int key, int modifiers) const {
    if (m_recording) return false;
    const auto action = std::find_if(m_actions.cbegin(), m_actions.cend(), [&](const Action &action) { return action.id == id; });
    if (action == m_actions.cend()) return false;
    const QKeySequence pressed{QKeyCombination{Qt::KeyboardModifiers(modifiers), Qt::Key(key)}};
    for (const auto &binding : m_custom.value(id, action->defaults).toStringList()) {
        if (isStylusInput(binding)) continue;
        const auto sequence = QKeySequence::fromString(binding, QKeySequence::PortableText);
        if (sequence == pressed) return true;
        if (key == Qt::Key_Plus && sequence == QKeySequence(QKeyCombination(Qt::KeyboardModifiers(modifiers & ~Qt::ShiftModifier), Qt::Key(key)))) return true;
    }
    return false;
}
