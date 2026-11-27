#include "../../../src/app/keybindings.h"
#include "terminalitem.h"

#include <QClipboard>
#include <QCursor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QInputMethod>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QTextCharFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int terminalPadding = 14;
constexpr int cornerRadius = 14;
constexpr int maximumScrollbackLines = 2000;

VTermModifier terminalModifiers(Qt::KeyboardModifiers modifiers)
{
    int result = VTERM_MOD_NONE;
    if (modifiers.testFlag(Qt::ShiftModifier))
        result |= VTERM_MOD_SHIFT;
    if (modifiers.testFlag(Qt::AltModifier))
        result |= VTERM_MOD_ALT;
    if (modifiers.testFlag(Qt::ControlModifier))
        result |= VTERM_MOD_CTRL;
    return static_cast<VTermModifier>(result);
}
}

TerminalItem::TerminalItem(QQuickItem *parent)
    : QQuickPaintedItem(parent)
    , m_terminal(vterm_new(24, 80))
    , m_font(QFontDatabase::systemFont(QFontDatabase::FixedFont))
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setActiveFocusOnTab(true);
    setFlag(ItemAcceptsInputMethod, true);
    setAntialiasing(true);
    setCursor(QCursor(Qt::IBeamCursor));

    vterm_set_utf8(m_terminal, 1);
    vterm_output_set_callback(m_terminal, &TerminalItem::outputCallback, this);
    m_screen = vterm_obtain_screen(m_terminal);

    static const VTermScreenCallbacks callbacks{
        &TerminalItem::damageCallback,
        &TerminalItem::moveRectCallback,
        &TerminalItem::moveCursorCallback,
        &TerminalItem::setTermPropertyCallback,
        &TerminalItem::bellCallback,
        &TerminalItem::resizeCallback,
        &TerminalItem::pushScrollbackCallback,
        &TerminalItem::popScrollbackCallback,
        &TerminalItem::clearScrollbackCallback,
    };
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    static const VTermStateFallbacks fallbacks{
        nullptr,
        nullptr,
        &TerminalItem::oscFallback,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
    };
    vterm_screen_set_unrecognised_fallbacks(m_screen, &fallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCREEN);
    vterm_screen_enable_altscreen(m_screen, true);
    vterm_screen_enable_reflow(m_screen, true);
    updateDefaultColors();
    vterm_screen_reset(m_screen, true);
}

TerminalItem::~TerminalItem()
{
    vterm_free(m_terminal);
}

bool TerminalItem::active() const
{
    return m_active;
}

void TerminalItem::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (!active) {
        m_preeditText.clear();
        m_preeditAttributes.clear();
        setControlModifier(false);
    }
    if (hasActiveFocus()) {
        if (!active)
            QGuiApplication::inputMethod()->reset();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
    update();
    emit activeChanged();
}

bool TerminalItem::hasContent() const
{
    return m_hasContent;
}

Qt::InputMethodHints TerminalItem::inputMethodHints() const
{
    return Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhSensitiveData
        | Qt::ImhMultiLine | Qt::ImhNoFullscreen;
}

qreal TerminalItem::minimumViewportHeight() const
{
    return terminalPadding * 2 + std::ceil(QFontMetricsF(m_font).height());
}

void TerminalItem::setControlModifier(bool enabled)
{
    if (m_controlModifier == enabled)
        return;
    m_controlModifier = enabled;
    emit controlModifierChanged();
}

QColor TerminalItem::foregroundColor() const
{
    return m_foregroundColor;
}

void TerminalItem::setForegroundColor(const QColor &color)
{
    if (m_foregroundColor == color)
        return;
    m_foregroundColor = color;
    updateDefaultColors();
    update();
    emit foregroundColorChanged();
}

QColor TerminalItem::backgroundColor() const
{
    return m_backgroundColor;
}

void TerminalItem::setBackgroundColor(const QColor &color)
{
    if (m_backgroundColor == color)
        return;
    m_backgroundColor = color;
    updateDefaultColors();
    update();
    emit backgroundColorChanged();
}

QColor TerminalItem::accentColor() const
{
    return m_accentColor;
}

void TerminalItem::setAccentColor(const QColor &color)
{
    if (m_accentColor == color)
        return;
    m_accentColor = color;
    update();
    emit accentColorChanged();
}

QColor TerminalItem::borderColor() const
{
    return m_borderColor;
}

void TerminalItem::setBorderColor(const QColor &color)
{
    if (m_borderColor == color)
        return;
    m_borderColor = color;
    update();
    emit borderColorChanged();
}

void TerminalItem::feed(const QByteArray &data)
{
    if (data.isEmpty())
        return;
    setHasContent(true);
    vterm_input_write(m_terminal, data.constData(), static_cast<size_t>(data.size()));
    vterm_screen_flush_damage(m_screen);
    if (hasActiveFocus())
        QGuiApplication::inputMethod()->update(Qt::ImQueryInput);
    update();
}

void TerminalItem::reset()
{
    clearSelection();
    setControlModifier(false);
    m_preeditText.clear();
    m_preeditAttributes.clear();
    if (hasActiveFocus())
        QGuiApplication::inputMethod()->reset();
    m_scrollback.clear();
    m_scrollOffset = 0;
    m_scrollWheelRemainder = 0;
    m_scrollPixelRemainder = 0;
    m_cursorPosition = {0, 0};
    m_cursorVisible = true;
    setHasContent(false);
    vterm_screen_reset(m_screen, true);
    updateDefaultColors();
    update();
}

void TerminalItem::paint(QPainter *painter)
{
    const QRectF itemBounds = boundingRect();
    QPainterPath backgroundPath;
    backgroundPath.addRoundedRect(itemBounds, cornerRadius, cornerRadius);
    painter->setClipPath(backgroundPath);
    painter->fillPath(backgroundPath, m_backgroundColor);

    QFontMetricsF metrics(m_font);
    const qreal cellWidth = std::max<qreal>(1, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max<qreal>(1, std::ceil(metrics.height()));
    const qreal baselineOffset = (cellHeight - metrics.height()) / 2 + metrics.ascent();
    const int firstLine = m_scrollback.size() - m_scrollOffset;
    unsigned int lastStyle = 16;

    for (int row = 0; row < m_rows; ++row) {
        for (int column = 0; column < m_columns; ++column) {
            VTermScreenCell cell{};
            if (!documentCell(firstLine + row, column, cell) || cell.chars[0] == uint32_t(-1))
                continue;

            QColor foreground = terminalColor(cell.fg);
            QColor background = terminalColor(cell.bg);
            if (cell.attrs.reverse)
                std::swap(foreground, background);
            const QRectF cellBounds(terminalPadding + column * cellWidth,
                                    terminalPadding + row * cellHeight,
                                    cellWidth * std::max<int>(1, cell.width),
                                    cellHeight);
            if (!VTERM_COLOR_IS_DEFAULT_BG(&cell.bg) || cell.attrs.reverse)
                painter->fillRect(cellBounds, background);
            if (cellSelected(row, column)) {
                QColor selectionColor = m_accentColor;
                selectionColor.setAlphaF(0.55);
                painter->fillRect(cellBounds, selectionColor);
            }

            if (cell.attrs.conceal || cell.chars[0] == 0)
                continue;

            QString text;
            for (const uint32_t character : cell.chars) {
                if (character == 0)
                    break;
                const char32_t codepoint = static_cast<char32_t>(character);
                text.append(QString::fromUcs4(&codepoint, 1));
            }

            const bool underlined = cell.attrs.underline != VTERM_UNDERLINE_OFF;
            const unsigned int style = cell.attrs.bold | (cell.attrs.italic << 1)
                                       | (underlined << 2) | (cell.attrs.strike << 3);
            if (style != lastStyle) {
                QFont cellFont = m_font;
                cellFont.setBold(cell.attrs.bold);
                cellFont.setItalic(cell.attrs.italic);
                cellFont.setUnderline(underlined);
                cellFont.setStrikeOut(cell.attrs.strike);
                painter->setFont(cellFont);
                lastStyle = style;
            }
            painter->setPen(foreground);
            painter->drawText(QPointF(cellBounds.left(), cellBounds.top() + baselineOffset), text);
        }
    }

    if (m_active && m_cursorVisible && m_scrollOffset == 0 && m_preeditText.isEmpty()) {
        const QRectF cursorCell(terminalPadding + m_cursorPosition.col * cellWidth,
                                terminalPadding + m_cursorPosition.row * cellHeight,
                                cellWidth, cellHeight);
        QColor cursorColor = m_accentColor;
        cursorColor.setAlphaF(0.72);
        if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
            painter->fillRect(QRectF(cursorCell.left(), cursorCell.top(), 2, cursorCell.height()),
                              cursorColor);
        else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
            painter->fillRect(QRectF(cursorCell.left(), cursorCell.bottom() - 2,
                                     cursorCell.width(), 2), cursorColor);
        else
            painter->fillRect(cursorCell, cursorColor);
    }

    if (m_active && !m_preeditText.isEmpty() && m_scrollOffset == 0) {
        const QPointF origin(terminalPadding + m_cursorPosition.col * cellWidth,
                             terminalPadding + m_cursorPosition.row * cellHeight);
        QTextLayout layout(m_preeditText, m_font);
        QList<QTextLayout::FormatRange> formats;
        for (const auto &attribute : m_preeditAttributes) {
            if (attribute.type == QInputMethodEvent::TextFormat)
                formats.append({attribute.start, attribute.length,
                    qvariant_cast<QTextFormat>(attribute.value).toCharFormat()});
        }
        layout.setFormats(formats);
        layout.beginLayout();
        auto line = layout.createLine();
        line.setLineWidth(std::max<qreal>(cellWidth, width() - terminalPadding - origin.x()));
        layout.endLayout();
        painter->fillRect(QRectF(origin, QSizeF(line.naturalTextWidth(), cellHeight)), m_backgroundColor);
        painter->setPen(m_foregroundColor);
        layout.draw(painter, origin);
        if (m_preeditCursorVisible) {
            painter->setPen(m_accentColor);
            layout.drawCursor(painter, origin, m_preeditCursor, 2);
        }
    }

    painter->setClipping(false);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(m_borderColor);
    painter->drawRoundedRect(itemBounds.adjusted(0.5, 0.5, -0.5, -0.5),
                             cornerRadius, cornerRadius);
}

void TerminalItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    updateTerminalSize();
}

void TerminalItem::keyPressEvent(QKeyEvent *event)
{
    if (!m_active || !m_terminal) {
        event->ignore();
        return;
    }

    const Qt::KeyboardModifiers qtModifiers = event->modifiers()
        | (m_controlModifier ? Qt::ControlModifier : Qt::NoModifier);
    setControlModifier(false);
    const VTermModifier modifiers = terminalModifiers(qtModifiers);

    const auto bindings = KeyBindings::instance();
    const auto matches = [&](const char *id) { return bindings->matches(QLatin1String(id), event->key(), int(qtModifiers)); };
    if (matches("terminalCopy") || (m_hasSelection && event->key() == Qt::Key_C
                                   && qtModifiers == Qt::ControlModifier)) {
        copySelection();
        event->accept();
        return;
    }
    if (matches("zoomIn") || matches("zoomOut")) {
        zoomFont(matches("zoomOut") ? -1 : 1); event->accept(); return;
    }
    if (const bool paste = matches("terminalPaste"); paste || matches("terminalDeleteWord")) {
        m_scrollOffset = 0;
        clearSelection();
        if (paste)
            pasteClipboard();
        else
            vterm_keyboard_unichar(m_terminal, 0x17, VTERM_MOD_NONE);
        event->accept();
        return;
    }

    VTermKey terminalKey = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: terminalKey = VTERM_KEY_ENTER; break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: terminalKey = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: terminalKey = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: terminalKey = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: terminalKey = VTERM_KEY_UP; break;
    case Qt::Key_Down: terminalKey = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: terminalKey = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: terminalKey = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: terminalKey = VTERM_KEY_INS; break;
    case Qt::Key_Delete: terminalKey = VTERM_KEY_DEL; break;
    case Qt::Key_Home: terminalKey = VTERM_KEY_HOME; break;
    case Qt::Key_End: terminalKey = VTERM_KEY_END; break;
    case Qt::Key_PageUp: terminalKey = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: terminalKey = VTERM_KEY_PAGEDOWN; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35)
            terminalKey = static_cast<VTermKey>(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        break;
    }

    const bool controlLetter = qtModifiers.testFlag(Qt::ControlModifier)
        && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z;
    if (terminalKey == VTERM_KEY_NONE && !controlLetter && event->text().isEmpty()) {
        event->ignore();
        return;
    }

    m_scrollOffset = 0;
    clearSelection();
    if (terminalKey != VTERM_KEY_NONE) {
        vterm_keyboard_key(m_terminal, terminalKey, modifiers);
    } else if (controlLetter) {
        vterm_keyboard_unichar(m_terminal,
                              static_cast<uint32_t>('a' + event->key() - Qt::Key_A),
                              modifiers);
    } else {
        sendText(event->text(), modifiers);
    }
    event->accept();
    update();
}

void TerminalItem::inputMethodEvent(QInputMethodEvent *event)
{
    if (!m_active) {
        event->ignore();
        return;
    }

    // Android Backspace arrives as an empty commit replacing surrounding text.
    const int start = event->replacementStart();
    const int length = event->replacementLength();
    const auto [surrounding, cursor] = inputMethodLine();
    const auto characterCount = [&](int position, int count) {
        return surrounding.mid(position, count).toUcs4().size();
    };
    const int left = std::min(std::max(0, -start), length);
    const int offset = start + left;
    const int moveStart = offset < 0 ? cursor + offset : cursor;
    const int moveCount = characterCount(moveStart, std::abs(offset));
    const int backspaceCount = characterCount(cursor + start, left);
    const int deleteCount = characterCount(cursor + start + left, length - left);
    for (int index = 0; index < moveCount; ++index)
        vterm_keyboard_key(m_terminal, offset < 0 ? VTERM_KEY_LEFT : VTERM_KEY_RIGHT, VTERM_MOD_NONE);
    for (int index = 0; index < backspaceCount; ++index)
        vterm_keyboard_key(m_terminal, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
    for (int index = 0; index < deleteCount; ++index)
        vterm_keyboard_key(m_terminal, VTERM_KEY_DEL, VTERM_MOD_NONE);

    for (const auto character : event->commitString().toUcs4()) {
        if (character == '\n' || character == '\r')
            vterm_keyboard_key(m_terminal, VTERM_KEY_ENTER, VTERM_MOD_NONE);
        else
            vterm_keyboard_unichar(m_terminal, character,
                                  m_controlModifier ? VTERM_MOD_CTRL : VTERM_MOD_NONE);
        setControlModifier(false);
    }
    m_preeditText = event->preeditString();
    m_preeditAttributes = event->attributes();
    m_preeditCursor = m_preeditText.size();
    m_preeditCursorVisible = true;
    for (const auto &attribute : m_preeditAttributes) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            m_preeditCursor = std::clamp(attribute.start, 0, int(m_preeditText.size()));
            m_preeditCursorVisible = attribute.length != 0;
        }
    }
    if (!event->commitString().isEmpty() || !m_preeditText.isEmpty()
        || moveCount > 0 || backspaceCount > 0 || deleteCount > 0) {
        m_scrollOffset = 0;
        clearSelection();
    }
    update();
    event->accept();
}

QPair<QString, int> TerminalItem::inputMethodLine() const
{
    QString text;
    int cursor = 0;
    for (int column = 0; column < m_columns; ++column) {
        if (column == m_cursorPosition.col)
            cursor = text.size();
        VTermScreenCell cell{};
        if (!vterm_screen_get_cell(m_screen, {m_cursorPosition.row, column}, &cell))
            continue;
        if (cell.chars[0] == uint32_t(-1))
            continue; // Continuation of a wide character.
        if (cell.chars[0] == 0) {
            text.append(QLatin1Char(' '));
            continue;
        }
        for (const uint32_t character : cell.chars) {
            if (character == 0)
                break;
            const char32_t codepoint = character;
            text.append(QString::fromUcs4(&codepoint, 1));
        }
    }
    if (m_cursorPosition.col >= m_columns)
        cursor = text.size();
    return {text, cursor};
}

QVariant TerminalItem::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImEnabled)
        return m_active;
    if (query == Qt::ImFont)
        return m_font;
    if (query == Qt::ImHints)
        return int(inputMethodHints());
    if (query == Qt::ImEnterKeyType)
        return Qt::EnterKeyDefault; // Qt Android applies multiline Return only for the default enter type.
    if (query == Qt::ImCurrentSelection)
        return QString{};
    if (query == Qt::ImSurroundingText || query == Qt::ImCursorPosition
        || query == Qt::ImAnchorPosition || query == Qt::ImAbsolutePosition
        || query == Qt::ImTextBeforeCursor || query == Qt::ImTextAfterCursor) {
        const auto [text, cursor] = inputMethodLine();
        if (query == Qt::ImSurroundingText)
            return text;
        if (query == Qt::ImTextBeforeCursor)
            return text.left(cursor);
        if (query == Qt::ImTextAfterCursor)
            return text.mid(cursor);
        return cursor;
    }
    if (query == Qt::ImCursorRectangle) {
        const QFontMetricsF metrics(m_font);
        const qreal cellWidth = std::max<qreal>(1, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
        const qreal cellHeight = std::max<qreal>(1, std::ceil(metrics.height()));
        return QRectF(terminalPadding + m_cursorPosition.col * cellWidth
                          + metrics.horizontalAdvance(m_preeditText.left(m_preeditCursor)),
                      terminalPadding + m_cursorPosition.row * cellHeight,
                      cellWidth, cellHeight);
    }
    return QQuickPaintedItem::inputMethodQuery(query);
}

void TerminalItem::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton) {
        event->ignore();
        return;
    }
    if (event->source() != Qt::MouseEventNotSynthesized
        && event->pointingDevice()->type() == QInputDevice::DeviceType::TouchScreen) {
        event->ignore();
        return; // Touch handlers distinguish taps, scrolling and long-press selection.
    }
    forceActiveFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::LeftButton && m_active && hasActiveFocus())
        QGuiApplication::inputMethod()->show();
    if (event->button() == Qt::MiddleButton) {
        pastePrimarySelection();
        event->accept();
        return;
    }

    m_selectionAnchor = selectionPosition(event->position());
    m_selectionEnd = m_selectionAnchor;
    m_selecting = true;
    m_hasSelection = false;
    emit selectionChanged();
    update();
    event->accept();
}

void TerminalItem::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting || !(event->buttons() & Qt::LeftButton)) {
        event->ignore();
        return;
    }

    m_selectionEnd = selectionPosition(event->position());
    m_hasSelection = m_selectionEnd != m_selectionAnchor;
    emit selectionChanged();
    update();
    event->accept();
}

void TerminalItem::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_selecting) {
        event->ignore();
        return;
    }

    m_selectionEnd = selectionPosition(event->position());
    m_hasSelection = m_selectionEnd != m_selectionAnchor;
    emit selectionChanged();
    m_selecting = false;
    if (m_hasSelection && QGuiApplication::clipboard()->supportsSelection())
        copySelection(QClipboard::Selection);
    update();
    event->accept();
}

void TerminalItem::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    forceActiveFocus(Qt::MouseFocusReason);
    selectWordAt(event->position());
    event->accept();
}

void TerminalItem::selectWordAt(const QPointF &point)
{
    const SelectionPosition clicked = selectionPosition(point);
    VTermScreenCell cell{};
    if (!documentCell(clicked.line, clicked.column, cell) || cell.chars[0] == 0) {
        clearSelection();
        return;
    }

    const auto isWordCell = [this, line = clicked.line](int column) {
        VTermScreenCell candidate{};
        if (!documentCell(line, column, candidate) || candidate.chars[0] == 0)
            return false;
        const char32_t character = static_cast<char32_t>(candidate.chars[0]);
        return !QString::fromUcs4(&character, 1).at(0).isSpace();
    };

    int first = clicked.column;
    int last = clicked.column;
    while (first > 0 && isWordCell(first - 1))
        --first;
    while (last + 1 < m_columns && isWordCell(last + 1))
        ++last;

    m_selectionAnchor = {clicked.line, first};
    m_selectionEnd = {clicked.line, last};
    m_selecting = false;
    m_hasSelection = true;
    emit selectionChanged();
    if (QGuiApplication::clipboard()->supportsSelection())
        copySelection(QClipboard::Selection);
    update();
}

void TerminalItem::selectAll()
{
    m_selectionAnchor = {0, 0};
    m_selectionEnd = {int(m_scrollback.size()) + m_rows - 1, m_columns - 1};
    m_hasSelection = true;
    m_selecting = false;
    emit selectionChanged();
    update();
}

void TerminalItem::sendKey(int key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    keyPressEvent(&event);
}

void TerminalItem::scrollFrom(int start, qreal displacement)
{
    const qreal lineHeight = std::max<qreal>(1, std::ceil(QFontMetricsF(m_font).height()));
    m_scrollOffset = std::clamp(start + int(std::round(displacement / lineHeight)),
                              0, int(m_scrollback.size()));
    clearSelection();
    update();
}

void TerminalItem::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::ShiftModifier)) {
        int delta = event->angleDelta().y();
        if (delta == 0)
            delta = event->pixelDelta().y() * 3;
        if (delta == 0) {
            event->ignore();
            return;
        }
        m_zoomWheelDelta += delta;
        const int steps = m_zoomWheelDelta / 120;
        if (steps != 0) {
            zoomFont(steps);
            m_zoomWheelDelta -= steps * 120;
        }
        event->accept();
        return;
    }

    if (event->phase() == Qt::ScrollBegin)
        m_scrollPixelRemainder = 0;
    const int pixels = event->pixelDelta().y();
    const int angle = event->angleDelta().y();
    if (pixels == 0 && angle == 0) {
        event->ignore();
        return;
    }
    int lines = 0;
    if (pixels != 0) {
        const qreal lineHeight = std::max<qreal>(1, std::ceil(QFontMetricsF(m_font).height()));
        m_scrollPixelRemainder += pixels / lineHeight;
        lines = static_cast<int>(m_scrollPixelRemainder);
        m_scrollPixelRemainder -= lines;
    } else {
        constexpr int anglePerLine = 40;
        m_scrollWheelRemainder += angle;
        lines = m_scrollWheelRemainder / anglePerLine;
        m_scrollWheelRemainder %= anglePerLine;
    }
    if (lines != 0) {
        m_scrollOffset = std::clamp(m_scrollOffset + lines, 0,
                                    static_cast<int>(m_scrollback.size()));
        update();
    }
    event->accept();
}

void TerminalItem::outputCallback(const char *data, size_t length, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    emit terminal->dataGenerated(QByteArray(data, static_cast<qsizetype>(length)));
}

int TerminalItem::damageCallback(VTermRect, void *user)
{
    static_cast<TerminalItem *>(user)->update();
    return 1;
}

int TerminalItem::moveRectCallback(VTermRect, VTermRect, void *user)
{
    static_cast<TerminalItem *>(user)->update();
    return 1;
}

int TerminalItem::moveCursorCallback(VTermPos position, VTermPos, int visible, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    terminal->m_cursorPosition = position;
    terminal->m_cursorVisible = visible;
    terminal->update();
    return 1;
}

int TerminalItem::setTermPropertyCallback(VTermProp property, VTermValue *value, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    if (property == VTERM_PROP_CURSORVISIBLE)
        terminal->m_cursorVisible = value->boolean;
    else if (property == VTERM_PROP_CURSORSHAPE)
        terminal->m_cursorShape = value->number;
    terminal->update();
    return 1;
}

int TerminalItem::bellCallback(void *)
{
    return 1;
}

int TerminalItem::resizeCallback(int rows, int columns, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    terminal->m_rows = rows;
    terminal->m_columns = columns;
    terminal->update();
    return 1;
}

int TerminalItem::pushScrollbackCallback(int columns, const VTermScreenCell *cells, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    TerminalLine line(columns);
    std::copy_n(cells, columns, line.begin());
    terminal->m_scrollback.append(std::move(line));
    if (terminal->m_scrollOffset > 0)
        ++terminal->m_scrollOffset;
    if (terminal->m_scrollback.size() > maximumScrollbackLines) {
        terminal->m_scrollback.removeFirst();
        if (terminal->m_hasSelection || terminal->m_selecting) {
            --terminal->m_selectionAnchor.line;
            --terminal->m_selectionEnd.line;
            if (terminal->m_selectionAnchor.line < 0 || terminal->m_selectionEnd.line < 0)
                terminal->clearSelection();
        }
    }
    terminal->m_scrollOffset = std::min(terminal->m_scrollOffset,
                                        static_cast<int>(terminal->m_scrollback.size()));
    return 1;
}

int TerminalItem::popScrollbackCallback(int columns, VTermScreenCell *cells, void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    if (terminal->m_scrollback.isEmpty())
        return 0;

    const TerminalLine line = terminal->m_scrollback.takeLast();
    VTermScreenCell blank{};
    blank.width = 1; // libvterm advances by cell width while restoring scrollback.
    vterm_state_get_default_colors(vterm_obtain_state(terminal->m_terminal), &blank.fg, &blank.bg);
    std::fill_n(cells, columns, blank);
    std::copy_n(line.cbegin(), std::min(columns, static_cast<int>(line.size())), cells);
    return 1;
}

int TerminalItem::clearScrollbackCallback(void *user)
{
    auto *terminal = static_cast<TerminalItem *>(user);
    terminal->m_scrollback.clear();
    terminal->m_scrollOffset = 0;
    terminal->m_scrollWheelRemainder = 0;
    terminal->m_scrollPixelRemainder = 0;
    terminal->clearSelection();
    terminal->update();
    return 1;
}
int TerminalItem::oscFallback(int command, VTermStringFragment fragment, void *user)
{
    if ((command != 10 && command != 11) || !fragment.final)
        return 0;

    const QByteArray request(fragment.str, static_cast<qsizetype>(fragment.len));
    if (!request.contains('?'))
        return 0;

    auto *terminal = static_cast<TerminalItem *>(user);
    const QColor color = command == 10 ? terminal->m_foregroundColor
                                       : terminal->m_backgroundColor;
    const QByteArray response = QString::asprintf("\x1b]%d;rgb:%04x/%04x/%04x\x1b\\",
                                                   command,
                                                   color.red() * 257,
                                                   color.green() * 257,
                                                   color.blue() * 257).toLatin1();
    emit terminal->dataGenerated(response);
    return 1;
}

void TerminalItem::updateTerminalSize()
{
    if (!m_terminal || width() <= terminalPadding * 2 || height() <= terminalPadding * 2)
        return;

    const QFontMetricsF metrics(m_font);
    const qreal cellWidth = std::max<qreal>(1, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max<qreal>(1, std::ceil(metrics.height()));
    const int columns = std::max(2, static_cast<int>((width() - terminalPadding * 2) / cellWidth));
    const int rows = std::max(1, static_cast<int>((height() - terminalPadding * 2) / cellHeight));
    if (rows == m_rows && columns == m_columns)
        return;

    // Closing the keyboard changes rows without changing document positions.
    if (columns != m_columns)
        clearSelection();
    m_rows = rows;
    m_columns = columns;
    vterm_set_size(m_terminal, rows, columns);
    emit terminalSizeChanged(rows, columns);
    update();
}

void TerminalItem::updateDefaultColors()
{
    if (!m_screen)
        return;
    VTermColor foreground{};
    VTermColor background{};
    vterm_color_rgb(&foreground, static_cast<uint8_t>(m_foregroundColor.red()),
                    static_cast<uint8_t>(m_foregroundColor.green()),
                    static_cast<uint8_t>(m_foregroundColor.blue()));
    vterm_color_rgb(&background, static_cast<uint8_t>(m_backgroundColor.red()),
                    static_cast<uint8_t>(m_backgroundColor.green()),
                    static_cast<uint8_t>(m_backgroundColor.blue()));
    vterm_screen_set_default_colors(m_screen, &foreground, &background);
}

void TerminalItem::zoomFont(int steps)
{
    qreal currentSize = m_font.pointSizeF();
    if (currentSize <= 0)
        currentSize = 10;
    const qreal updatedSize = std::clamp(currentSize + steps, 6.0, 32.0);
    if (qFuzzyCompare(currentSize, updatedSize))
        return;
    m_font.setPointSizeF(updatedSize);
    emit fontMetricsChanged();
    m_scrollOffset = 0;
    updateTerminalSize();
    update();
}

void TerminalItem::setHasContent(bool hasContent)
{
    if (m_hasContent == hasContent)
        return;
    m_hasContent = hasContent;
    emit hasContentChanged();
}

void TerminalItem::sendText(const QString &text, VTermModifier modifiers)
{
    const QList<uint> characters = text.toUcs4();
    for (const uint character : characters)
        vterm_keyboard_unichar(m_terminal, character, modifiers);
}

void TerminalItem::pasteClipboard()
{
    if (!m_active)
        return;
    QString text = QGuiApplication::clipboard()->text();
    text.remove(QLatin1Char('\0'));
    if (text.isEmpty())
        return;
    vterm_keyboard_start_paste(m_terminal);
    m_scrollOffset = 0;
    clearSelection();
    sendText(text);
    vterm_keyboard_end_paste(m_terminal);
}

bool TerminalItem::clipboardHasText() const
{
    return !QGuiApplication::clipboard()->text().isEmpty();
}

void TerminalItem::pastePrimarySelection()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    QString text = clipboard->supportsSelection() ? clipboard->text(QClipboard::Selection)
                                                   : clipboard->text();
    text.remove(QLatin1Char('\0'));
    if (text.isEmpty())
        return;
    vterm_keyboard_start_paste(m_terminal);
    sendText(text);
    vterm_keyboard_end_paste(m_terminal);
}

void TerminalItem::clearSelection()
{
    if (!m_hasSelection && !m_selecting)
        return;
    m_hasSelection = false;
    m_selecting = false;
    emit selectionChanged();
    update();
}

void TerminalItem::copySelection(QClipboard::Mode mode) const
{
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text, mode);
}

TerminalItem::SelectionPosition TerminalItem::selectionPosition(const QPointF &point) const
{
    const QFontMetricsF metrics(m_font);
    const qreal cellWidth = std::max<qreal>(1, std::ceil(metrics.horizontalAdvance(QLatin1Char('M'))));
    const qreal cellHeight = std::max<qreal>(1, std::ceil(metrics.height()));
    const int visibleRow = std::clamp(static_cast<int>((point.y() - terminalPadding) / cellHeight),
                                      0, m_rows - 1);
    const int column = std::clamp(static_cast<int>((point.x() - terminalPadding) / cellWidth),
                                  0, m_columns - 1);
    return {static_cast<int>(m_scrollback.size()) - m_scrollOffset + visibleRow, column};
}

QString TerminalItem::selectedText() const
{
    if (!m_hasSelection)
        return {};

    const SelectionPosition first = std::min(m_selectionAnchor, m_selectionEnd);
    const SelectionPosition last = std::max(m_selectionAnchor, m_selectionEnd);
    QStringList lines;
    lines.reserve(last.line - first.line + 1);

    for (int line = first.line; line <= last.line; ++line) {
        const int firstColumn = line == first.line ? first.column : 0;
        const int lastColumn = line == last.line ? last.column : m_columns - 1;
        QString text;
        for (int column = firstColumn; column <= lastColumn; ++column) {
            VTermScreenCell cell{};
            if (!documentCell(line, column, cell) || cell.chars[0] == 0) {
                text.append(QLatin1Char(' '));
                continue;
            }
            if (cell.chars[0] == uint32_t(-1))
                continue;
            for (const uint32_t value : cell.chars) {
                if (value == 0)
                    break;
                const char32_t character = static_cast<char32_t>(value);
                text.append(QString::fromUcs4(&character, 1));
            }
        }
        while (text.endsWith(QLatin1Char(' ')))
            text.chop(1);
        lines.append(text);
    }
    return lines.join(QLatin1Char('\n'));
}

bool TerminalItem::cellSelected(int visibleRow, int column) const
{
    if (!m_hasSelection)
        return false;
    const SelectionPosition position{
        static_cast<int>(m_scrollback.size()) - m_scrollOffset + visibleRow, column};
    const SelectionPosition first = std::min(m_selectionAnchor, m_selectionEnd);
    const SelectionPosition last = std::max(m_selectionAnchor, m_selectionEnd);
    return position >= first && position <= last;
}

bool TerminalItem::documentCell(int line, int column, VTermScreenCell &cell) const
{
    if (line < 0 || column < 0 || column >= m_columns)
        return false;
    if (line < m_scrollback.size()) {
        if (column >= m_scrollback.at(line).size())
            return false;
        cell = m_scrollback.at(line).at(column);
        return true;
    }
    const int screenRow = line - m_scrollback.size();
    if (screenRow < 0 || screenRow >= m_rows)
        return false;
    return vterm_screen_get_cell(m_screen, {screenRow, column}, &cell);
}

QColor TerminalItem::terminalColor(VTermColor color) const
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&color))
        return m_foregroundColor;
    if (VTERM_COLOR_IS_DEFAULT_BG(&color))
        return m_backgroundColor;

    vterm_screen_convert_color_to_rgb(m_screen, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}
