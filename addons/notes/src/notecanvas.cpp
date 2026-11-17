#include "notecanvas.h"
#include "notedocument.h"
#include <QQuickWindow>
#include <QTabletEvent>
#include <QScreen>
#include <QDebug>
#include <QTime>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QWheelEvent>
#include <QGuiApplication>
#include <QClipboard>
#include <QMimeData>
#include <QJsonDocument>
#include <QStyleHints>
#include <numbers>
#include <QTouchEvent>
#include <QNativeGestureEvent>
#include <QTextBlock>
#include <QFontMetricsF>
#include <QImageReader>
#include <QTextCursor>
#include <QAbstractTextDocumentLayout>
#include <algorithm>
#include <cmath>

void NoteTextLayout::setDocument(QQuickTextDocument *document) {
    if (document == m_document) return;
    if (m_document) disconnect(m_document->textDocument(), nullptr, this, nullptr);
    m_document = document;
    if (m_document) {
        connect(m_document->textDocument(), &QTextDocument::contentsChanged, this, &NoteTextLayout::apply);
        connect(m_document->textDocument()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, &NoteTextLayout::layoutChanged);
    }
    apply(); emit changed();
}
void NoteTextLayout::setLineSpacing(qreal spacing) {
    if (spacing == m_spacing) return;
    m_spacing = std::clamp(spacing, 12., 160.); apply(); emit changed();
}
void NoteTextLayout::apply() {
    if (!m_document || m_applying) return;
    m_applying = true;
    auto document = m_document->textDocument();
    document->setDocumentMargin(0);
    QTextCursor edit(document); edit.joinPreviousEditBlock();
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        qreal height = QFontMetricsF(document->defaultFont()).height();
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid()) height = std::max(height, QFontMetricsF(fragment.charFormat().font().resolve(document->defaultFont())).height());
        }
        const qreal spacing = std::ceil(height / m_spacing) * m_spacing;
        auto format = block.blockFormat();
        if (format.lineHeight() == spacing && format.lineHeightType() == QTextBlockFormat::FixedHeight
            && format.topMargin() == 0 && format.bottomMargin() == 0) continue;
        format.setLineHeight(spacing, QTextBlockFormat::FixedHeight);
        format.setTopMargin(0); format.setBottomMargin(0);
        QTextCursor cursor(block); cursor.setBlockFormat(format);
    }
    edit.endEditBlock(); m_applying = false; emit layoutChanged();
}
qreal NoteTextLayout::firstLineHeight() const {
    return m_document ? std::max(m_spacing, m_document->textDocument()->begin().blockFormat().lineHeight()) : m_spacing;
}
qreal NoteTextLayout::firstBaseline() const {
    if (!m_document) return 0;
    auto document = m_document->textDocument();
    const auto block = document->begin();
    const auto bounds = document->documentLayout()->blockBoundingRect(block);
    const auto layout = block.layout();
    if (!layout || layout->lineCount() == 0) return 0;
    const auto line = layout->lineAt(0);
    return bounds.top() + line.y() + line.ascent();
}

void NoteTextFormat::setDocument(QQuickTextDocument *document) {
    if (m_document == document) return;
    if (m_document) disconnect(m_document->textDocument(), nullptr, this, nullptr);
    m_document = document; m_active = false;
    if (m_document) connect(m_document->textDocument(), &QTextDocument::contentsChange, this, [this](int position, int removed, int added) {
        if (!m_active || m_applying || added <= 0 || position != m_expected) return;
        m_expected = position + added; m_applying = true;
        QTextCursor cursor(m_document->textDocument()); cursor.setPosition(position); cursor.setPosition(position + added, QTextCursor::KeepAnchor);
        QTextCharFormat format; format.setFont(m_font); if (m_colour.isValid()) format.setForeground(m_colour);
        cursor.joinPreviousEditBlock(); cursor.mergeCharFormat(format); cursor.endEditBlock();
        m_applying = false; emit changed();
    });
    emit changed();
}
void NoteTextFormat::setPosition(int position) {
    m_position = position;
    QTimer::singleShot(0, this, [this] { if (m_active && m_position != m_expected) { m_active = false; emit changed(); } });
    emit changed();
}

NoteCanvas::NoteCanvas(QQuickItem *parent) : QQuickPaintedItem(parent), m_paper(NoteDocument::defaultPaper()) {
    setAntialiasing(true);
    connect(this, &NoteCanvas::selectionChanged, this, &NoteCanvas::updateSelectionFrame);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptTouchEvents(false); refreshTouchSupport(); setAcceptHoverEvents(true); setCursor(Qt::ArrowCursor);
    connect(this, &QQuickItem::windowChanged, this, &NoteCanvas::refreshTouchSupport);
    m_shapeTimer.setSingleShot(true); m_shapeTimer.setInterval(400); m_shapeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_shapeTimer, &QTimer::timeout, this, &NoteCanvas::recognizeHeldStroke);
    m_contextHoldTimer.setInterval(16); m_contextHoldTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_contextHoldTimer, &QTimer::timeout, this, [this] {
        const bool stylus = m_holdSource == HoldSource::Stylus;
        const bool valid = stylus ? stylusOnCanvas() && !m_stylusMoved && m_stylusAction != StylusAction::Consumed
            : m_touchPointer && !m_touchPanned && !stylusOnCanvas()
                && (m_touchAction == TouchAction::Pan || m_touchAction == TouchAction::Select
                    || m_touchAction == TouchAction::Draw || m_touchAction == TouchAction::Text);
        if (!valid) { stopContextHold(); return; }
        m_holdProgress = std::clamp((qreal(m_holdElapsed.elapsed()) - 400.) / 500., 0., 1.);
        emit holdChanged();
        if (m_holdProgress < 1) return;
        const auto position = m_holdPosition;
        stopContextHold(); m_touchTapTimer.invalidate();
        if (stylus) {
            if (m_stylusAction == StylusAction::Draw || m_stylusAction == StylusAction::Erase) cancelStroke();
            else if (m_stylusAction == StylusAction::Select && m_editing) { const auto before = m_before; restoreSnapshot(before); endEdit(); }
            m_selecting = false; m_dragging = false; m_marquee = false;
            m_stylusAction = StylusAction::Consumed;
        } else { finishTouchEdit(true); m_touchAction = TouchAction::ContextMenu; }
        requestContextMenu(position);
    });
    // Proximity events go to the application, not the canvas or window.
    QCoreApplication::instance()->installEventFilter(this);
    const auto hidden = [this] { if (!isVisible() || !isEnabled()) finishInput(); else refreshTouchSupport(); };
    connect(this, &QQuickItem::visibleChanged, this, hidden);
    connect(this, &QQuickItem::enabledChanged, this, hidden);
}
void NoteCanvas::refreshTouchSupport() {
    const auto devices = QInputDevice::devices();
    const bool available = std::any_of(devices.cbegin(), devices.cend(), [](const QInputDevice *device) {
        return device->type() == QInputDevice::DeviceType::TouchScreen;
    });
    if (available == m_touchAvailable) return;
    cancelTouch(); m_touchAvailable = available; setAcceptTouchEvents(available); emit touchAvailableChanged();
}
void NoteCanvas::setDrawWithFinger(bool enabled) {
    if (enabled == m_drawWithFinger) return;
    cancelTouch(); m_drawWithFinger = enabled; emit toolChanged();
}
void NoteCanvas::setPressureSensitive(bool enabled) {
    if (enabled == m_pressureSensitive) return;
    finishStroke(); m_pressureSensitive = enabled; emit toolChanged();
}
void NoteCanvas::setStylusBindings(const QVariantMap &bindings) {
    if (bindings == m_stylusBindings) return;
    setStylusToolHeld({}); m_stylusBindings = bindings; emit stylusBindingsChanged();
}
void NoteCanvas::setStylusToolHeld(const QString &tool) {
    if (tool == m_stylusToolHeld) return;
    finishStylus(nullptr, false); m_stylusToolHeld = tool; emit stylusToolHeldChanged();
    updatePointer(m_pointer * m_zoom - QPointF(m_offsetX, m_offsetY)); update();
}
void NoteCanvas::setStylusDown(bool down) {
    if (m_stylusDown == down) return;
    m_stylusDown = down; emit stylusDownChanged();
}
void NoteCanvas::trackStylusDevice(const QPointingDevice *device) {
    if (!device || m_trackedStylusDevices.contains(device)) return;
    m_trackedStylusDevices.insert(device);
    connect(device, &QObject::destroyed, this, [this, device] {
        m_trackedStylusDevices.remove(device); m_pensInProximity.remove(device);
        // QPointer is already cleared when destroyed() is delivered. The active
        // gesture must be ended even when a digitizer disappears without lift.
        if (!m_stylusDevice || m_stylusDevice == device) { setStylusDown(false); finishStylus("device-removed"); }
        m_penTouchCooldown.restart();
    });
}
void NoteCanvas::setNotes(NotesManager *notes) {
    if (m_notes == notes) return;
    if (m_notes) { disconnect(m_notes, nullptr, this, nullptr); disconnect(this, nullptr, m_notes, nullptr); }
    m_notes = notes;
    if (notes) {
        const auto load = [this] { setDocumentId(m_notes->currentId()); setStrokes(m_notes->strokes()); setElements(m_notes->elements()); setPaper(m_notes->paper()); };
        connect(notes, &NotesManager::currentChanged, this, load);
        connect(notes, &NotesManager::strokesChanged, this, [this] { setStrokes(m_notes->strokes()); });
        connect(notes, &NotesManager::elementsChanged, this, [this] { setElements(m_notes->elements()); });
        connect(notes, &NotesManager::paperChanged, this, [this] { setPaper(m_notes->paper()); });
        connect(notes, &NotesManager::searchMatchesChanged, this, [this] { m_searchMatches = m_notes->searchMatches(); update(); });
        m_searchMatches = notes->searchMatches();
        connect(this, &NoteCanvas::strokesChanged, notes, [this] { if (m_documentId == m_notes->currentId()) m_notes->setStrokes(m_strokes); });
        connect(this, &NoteCanvas::elementsEdited, notes, [this] { m_notes->setElements(m_elements); });
        load();
    }
    emit notesChanged();
}
qreal NoteCanvas::paperScale() const { return m_zoom; }
void NoteCanvas::setViewLocked(bool locked) {
    if (m_viewLocked == locked) return;
    m_viewLocked = locked;
    if (locked && m_panning) { m_panning = false; setKeepMouseGrab(false); }
    updatePointer(m_pointer * m_zoom - QPointF(m_offsetX, m_offsetY));
    emit viewLockedChanged();
}
qreal NoteCanvas::documentWidth() const { return m_zoom * std::max({NoteDocument::paperSize(m_paper).width(), m_contentBounds.right() + 240, m_liveBounds.right() + 240}); }
qreal NoteCanvas::documentHeight() const { return m_zoom * std::max({NoteDocument::paperSize(m_paper).height(), m_contentBounds.bottom() + 240, m_liveBounds.bottom() + 240}); }
void NoteCanvas::moveView(qreal scale, const QPointF &from, const QPointF &to) {
    if (m_viewLocked || m_stylusDown) return;
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(from.x()) || !std::isfinite(from.y())
        || !std::isfinite(to.x()) || !std::isfinite(to.y())) return;
    stopContextHold();
    m_touchTapTimer.invalidate();
    finishStroke(); endEdit(); m_selecting = false; m_marquee = false; m_panning = false;
    if (stylusOnCanvas()) m_stylusAction = StylusAction::Consumed;
    const QPointF anchor = (QPointF(m_offsetX, m_offsetY) + from) / m_zoom;
    const QPointF pointer = m_pointer * m_zoom - QPointF(m_offsetX, m_offsetY);
    scale = std::clamp(scale, .25, 4.);
    if (scale != m_zoom) { m_zoom = scale; emit paperScaleChanged(); emit documentHeightChanged(); }
    emit panRequested(anchor.x() * scale - to.x() - m_offsetX, anchor.y() * scale - to.y() - m_offsetY);
    updatePointer(pointer); update();
}
void NoteCanvas::zoomAt(qreal factor, qreal x, qreal y) {
    if (m_viewLocked || m_stylusDown) return;
    if (!std::isfinite(factor) || factor <= 0 || !std::isfinite(x) || !std::isfinite(y)) return;
    if (m_touchEditing || m_touchAction == TouchAction::Text) cancelTouch();
    moveView(m_zoom * factor, QPointF(x, y), QPointF(x, y));
}
void NoteCanvas::setDrawEnabled(bool enabled) {
    if (m_drawEnabled == enabled) return;
    finishStroke(); m_drawEnabled = enabled;
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    emit toolChanged();
}
void NoteCanvas::setErasing(bool erase) { if (m_eraseTool != erase) { finishStroke(); m_eraseTool = erase; emit toolChanged(); } }
void NoteCanvas::setEraserMode(const QString &mode) {
    if ((mode == "stroke" || mode == "area") && m_eraserMode != mode) { finishStroke(); m_eraserMode = mode; emit toolChanged(); }
}
void NoteCanvas::setEraserSize(qreal size) { size = std::clamp(size, 4., 120.); if (m_eraserSize != size) { m_eraserSize = size; emit toolChanged(); update(); } }
void NoteCanvas::setTool(const QString &tool) {
    if (tool != "pointer" && tool != "pen" && tool != "eraser" && tool != "text" && tool != "pan") return;
    m_touchTapTimer.invalidate();
    stopContextHold();
    if (m_touchEditing || m_touchAction == TouchAction::Text) cancelTouch();
    finishStylus("tool-changed"); finishStroke(); endEdit(); m_tool = tool;
    setDrawEnabled(tool == "pen" || tool == "eraser"); setErasing(tool == "eraser");
    if (m_drawEnabled) clearSelection();
    updatePointer(m_pointer * m_zoom - QPointF(m_offsetX, m_offsetY)); emit toolChanged();
}
void NoteCanvas::updatePointer(const QPointF &point) {
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return;
    m_pointer = (point + QPointF(m_offsetX, m_offsetY)) / m_zoom;
    m_hovering = contains(point);
    const auto tool = m_stylusToolHeld.isEmpty() ? m_tool : m_stylusToolHeld;
    const bool drawing = m_stylusToolHeld.isEmpty() ? m_drawEnabled : tool == "pen" || tool == "eraser";
    const int handle = drawing && !m_touchPointer ? 0 : m_selecting && m_handle ? m_handle : handleAt(m_pointer);
    if (tool == "eraser" || m_stylusEraser) update();
    Qt::CursorShape resizeCursor = Qt::SizeFDiagCursor;
    if (handle > 0 && handle != 5) {
        const QPointF directions[] = {{-1,-1}, {1,-1}, {1,1}, {-1,1}, {}, {0,-1}, {1,0}, {0,1}, {-1,0}};
        const auto direction = m_frameTransform.map(directions[handle-1]) - m_frameTransform.map(QPointF{});
        const int orientation = (qRound(std::atan2(direction.y(), direction.x()) * 4 / std::numbers::pi) % 4 + 4) % 4;
        const Qt::CursorShape cursors[] = {Qt::SizeHorCursor, Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor};
        resizeCursor = cursors[orientation];
    }
    setCursor(m_touchPointer ? Qt::ArrowCursor : m_panning ? Qt::ClosedHandCursor : (tool == "eraser" || m_stylusEraser) ? Qt::BlankCursor : drawing ? Qt::CrossCursor : tool == "text" ? Qt::IBeamCursor
        : handle == 5 ? Qt::SizeAllCursor : handle > 0 ? resizeCursor : tool == "pan" ? (selectionContains(m_pointer) ? Qt::SizeAllCursor : m_viewLocked ? Qt::ArrowCursor : Qt::OpenHandCursor)
        : itemAt(m_pointer).isEmpty() ? Qt::ArrowCursor : Qt::SizeAllCursor);
    emit pointerChanged();
}
void NoteCanvas::hoverLeaveEvent(QHoverEvent *event) { leavePointer(); event->accept(); }
void NoteCanvas::hoverMoveEvent(QHoverEvent *event) { trackPointer(event->position().x(), event->position().y()); event->accept(); }
void NoteCanvas::handleWheel(QWheelEvent *event) {
    if (m_viewLocked || m_stylusDown) { event->accept(); return; }
    stopContextHold();
    const auto pixels = event->pixelDelta(); const auto angle = event->angleDelta();
    if (event->modifiers() & Qt::ControlModifier) {
        zoomAt(std::exp((pixels.isNull() ? angle.y() : pixels.y()) / 600.), event->position().x(), event->position().y());
        event->accept(); return;
    }
    qreal dx = pixels.isNull() ? angle.x() : pixels.x() * 1.5;
    qreal dy = pixels.isNull() ? angle.y() : pixels.y() * 1.5;
    if (event->modifiers() & Qt::ShiftModifier) { dx = dx != 0 ? dx : dy; dy = 0; }
    emit panRequested(-dx, -dy); event->accept();
}
void NoteCanvas::wheelEvent(QWheelEvent *event) { handleWheel(event); }
namespace {
bool tabletTipReleased(const QTabletEvent *event) {
    return event->type() == QEvent::TabletRelease
        && (event->button() == Qt::LeftButton || event->button() == Qt::NoButton);
}
bool stylusMouse(const QMouseEvent *event) {
    return event->deviceType() == QInputDevice::DeviceType::Stylus
        || event->pointerType() == QPointingDevice::PointerType::Pen
        || event->pointerType() == QPointingDevice::PointerType::Eraser;
}
bool touchMouse(const QMouseEvent *event) {
    return event->deviceType() == QInputDevice::DeviceType::TouchScreen;
}
}
void NoteCanvas::requestText(const QPointF &position) {
    const auto point = (position + QPointF(m_offsetX, m_offsetY)) / m_zoom;
    const auto key = itemAt(point);
    for (const auto &value : m_elements) {
        if (key == "e" + value.toMap().value("id").toString() && value.toMap().value("type") == "text") {
            emit editTextRequested(key.mid(1)); return;
        }
    }
    emit textRequested(point.x(), point.y());
}
void NoteCanvas::requestContextMenu(const QPointF &position) {
    finishStroke(); endEdit(); updatePointer(position);
    if (!selectionContains(m_pointer) && !m_selection.contains(itemAt(m_pointer))) selectAt(m_pointer.x(), m_pointer.y());
    emit contextMenuRequested(position.x(), position.y());
}
void NoteCanvas::mousePressEvent(QMouseEvent *event) {
    // Mouse events have no measured stylus pressure. Only handle real mouse
    // input here; tablet contact has its own pressure-driven path.
    if (touchMouse(event) || stylusMouse(event) || stylusOnCanvas() || m_touchPointer) { event->accept(); return; }
    m_touchTapTimer.invalidate();
    m_stylusEraser = false;
    updatePointer(event->position()); forceActiveFocus(); setKeepMouseGrab(true);
    if (event->button() == Qt::RightButton) {
        requestContextMenu(event->position());
    } else if (event->button() == Qt::MiddleButton || m_tool == "pan") {
        finishStroke();
        if (event->button() != Qt::MiddleButton && (selectionContains(m_pointer) || handleAt(m_pointer)))
            beginSelection(m_pointer, Qt::NoModifier, true);
        else if (!m_viewLocked) { m_panning = true; m_panPoint = event->position(); setCursor(Qt::ClosedHandCursor); }
    } else if (m_drawEnabled) beginStroke(event->position().x(), event->position().y(), 1, m_eraseTool);
    else if (m_tool == "text") requestText(event->position());
    else beginSelection(m_pointer, event->modifiers());
    event->accept();
}
bool NoteCanvas::selectionContains(const QPointF &point) const {
    return !m_selection.isEmpty() && m_frame.contains(m_frameTransform.inverted().map(point));
}
void NoteCanvas::beginSelection(const QPointF &point, Qt::KeyboardModifiers modifiers, bool selectedOnly) {
    m_press = point; m_handle = handleAt(point); m_selecting = true; m_dragging = false;
    const auto key = itemAt(point);
    const bool extend = modifiers & (Qt::ControlModifier | Qt::ShiftModifier);
    const bool inside = selectionContains(point);
    m_marquee = !selectedOnly && !inside && key.isEmpty() && m_handle == 0;
    m_marqueeBefore = extend ? m_selection : QStringList{};
    m_lasso = QPolygonF{point};
    if (!selectedOnly && m_handle == 0 && ((!inside && !m_selection.contains(key)) || extend))
        selectAt(point.x(), point.y(), int(modifiers));
    m_startBounds = m_frame; m_startFrame = m_frameTransform; m_startRotation = m_selectionRotation; beginEdit();
}
void NoteCanvas::updateSelection(const QPointF &point) {
    if (QLineF(m_press, point).length() * m_zoom >= QGuiApplication::styleHints()->startDragDistance()) m_dragging = true;
    if (m_dragging && m_marquee) {
        QPainterPath area;
        if (m_freeformSelection) {
            if (QLineF(m_lasso.last(), point).length() * m_zoom >= 2) m_lasso.append(point);
            area.addPolygon(m_lasso); area.closeSubpath();
        } else area.addRect(QRectF(m_press, point).normalized());
        m_selection = m_marqueeBefore;
        const auto select = [&](const QString &key, const QPainterPath &outline) {
            if (area.intersects(outline) && !m_selection.contains(key)) m_selection.append(key);
        };
        for (int i = 0; i < m_strokes.size(); ++i) {
            const auto stroke = m_strokes[i].toMap();
            if (!area.boundingRect().intersects(NoteDocument::strokeBounds(stroke))) continue;
            QPainterPath outline;
            if (m_freeformSelection) {
                QPainterPathStroker stroker; stroker.setWidth(std::max(1., stroke.value("width").toDouble()));
                stroker.setCapStyle(Qt::RoundCap); stroker.setJoinStyle(Qt::RoundJoin);
                outline = stroker.createStroke(NoteDocument::strokePath(stroke));
                if (outline.isEmpty()) outline.addEllipse(NoteDocument::strokeBounds(stroke));
            } else outline.addRect(NoteDocument::strokeBounds(stroke));
            select("s" + QString::number(i), outline);
        }
        for (const auto &value : m_elements) {
            const auto element = value.toMap(); QPainterPath outline;
            if (!area.boundingRect().intersects(NoteDocument::elementBounds(element))) continue;
            if (m_freeformSelection) {
                outline.addRect(QRectF(0, 0, element.value("width").toDouble(), element.value("height").toDouble()));
                outline = NoteDocument::elementTransform(element).map(outline);
            } else outline.addRect(NoteDocument::elementBounds(element));
            select("e" + element.value("id").toString(), outline);
        }
        emit selectionChanged(); update();
    } else if (m_dragging && !m_selection.isEmpty()) {
        QTransform transform; qreal sx = 1, sy = 1, degrees = 0;
        if (m_handle == 5) {
            const auto center = m_startFrame.map(m_startBounds.center()), a = m_press - center, b = point - center;
            degrees = (std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x())) * 180 / std::numbers::pi;
            const qreal absolute = m_startRotation + degrees;
            const qreal snapped = std::round(absolute / 45.) * 45.;
            if (std::abs(absolute - snapped) <= 5.) degrees = snapped - m_startRotation;
            transform.translate(center.x(), center.y()); transform.rotate(degrees); transform.translate(-center.x(), -center.y());
        } else if (m_handle) {
            const auto box = m_startBounds;
            const QPointF anchors[] = {box.bottomRight(), box.bottomLeft(), box.topLeft(), box.topRight(), {},
                {box.center().x(), box.bottom()}, {box.left(), box.center().y()}, {box.center().x(), box.top()}, {box.right(), box.center().y()}};
            const auto anchor = anchors[m_handle - 1];
            const auto a = m_startFrame.inverted().map(m_press) - anchor;
            const auto b = m_startFrame.inverted().map(point) - anchor;
            if (m_handle <= 4) sx = sy = std::clamp(QPointF::dotProduct(a, b) / std::max(1., QPointF::dotProduct(a, a)), .05, 20.);
            else if (m_handle == 6 || m_handle == 8) sy = std::clamp(b.y() / (std::abs(a.y()) < 1 ? 1 : a.y()), .05, 20.);
            else sx = std::clamp(b.x() / (std::abs(a.x()) < 1 ? 1 : a.x()), .05, 20.);
            QTransform local; local.translate(anchor.x(), anchor.y()); local.scale(sx, sy); local.translate(-anchor.x(), -anchor.y());
            transform = m_startFrame.inverted() * local * m_startFrame;
        } else transform.translate(point.x() - m_press.x(), point.y() - m_press.y());
        applyTransform(m_before, transform, sx, sy, degrees);
    }

}
void NoteCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (touchMouse(event) || stylusMouse(event) || stylusOnCanvas() || m_touchPointer) { event->accept(); return; }
    updatePointer(event->position());
    if (m_panning) { emit panRequested(m_panPoint.x() - event->position().x(), m_panPoint.y() - event->position().y()); m_panPoint = event->position(); }
    else if (m_drawing) addPoint(event->position().x(), event->position().y(), 1);
    else if (m_selecting) updateSelection(m_pointer);
    event->accept();
}
void NoteCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (touchMouse(event) || stylusMouse(event) || stylusOnCanvas() || m_touchPointer) { event->accept(); return; }
    if (m_drawing) { addPoint(event->position().x(), event->position().y(), 1); finishStroke(); }
    else if (m_selecting) {
        if (m_dragging) mouseMoveEvent(event);
        endEdit(); m_selecting = false; m_marquee = false; m_dragging = false; update();
    }
    m_panning = false; setKeepMouseGrab(false); updatePointer(event->position()); event->accept();
}
void NoteCanvas::mouseDoubleClickEvent(QMouseEvent *event) {
    if (touchMouse(event) || stylusMouse(event) || stylusOnCanvas() || m_touchPointer) { event->accept(); return; }
    if (m_tool == "pointer") {
        const auto key = itemAt((event->position() + QPointF(m_offsetX, m_offsetY)) / m_zoom);
        for (const auto &value : m_elements) if (key == "e" + value.toMap().value("id").toString() && value.toMap().value("type") == "text") {
            m_selecting = false; endEdit(); emit editTextRequested(key.mid(1)); event->accept(); return;
        }
    }
    mousePressEvent(event);
}
void NoteCanvas::mouseUngrabEvent() { if (!stylusOnCanvas() && !m_touchPointer) { finishStroke(); endEdit(); m_panning = false; m_selecting = false; m_marquee = false; update(); } }
bool NoteCanvas::ownsInputAt(const QPointF &scenePosition) const {
    return inputTargetAt(scenePosition) == this;
}
QQuickItem *NoteCanvas::inputTargetAt(const QPointF &scenePosition) const {
    if (!window() || !contains(mapFromScene(scenePosition))) return nullptr;
    // Follow stacking order, leaving controls, editors and popups above the
    // paper to Qt. Decorative items and hover-only overlays do not block ink.
    const auto hit = [&](auto &&self, QQuickItem *item) -> QQuickItem * {
        if (!item->isVisible() || !item->isEnabled()) return nullptr;
        if (item->objectName() == "noteCursorOverlay") return nullptr; // HoverHandler enables mouse acceptance, but never handles presses.
        const bool inside = item->contains(item->mapFromScene(scenePosition));
        if (item->clip() && !inside) return nullptr;
        auto children = item->childItems();
        // Qt's popup Overlay fills the window and accepts all mouse buttons,
        // even for a small tooltip. Menus need the overlay to receive outside
        // clicks so Qt can dismiss them before the canvas handles input.
        const bool overlay = item->inherits("QQuickOverlay");
        if (overlay) {
            for (auto child : children) {
                const auto popup = child->parent();
                if (child->isVisible() && popup && popup->inherits("QQuickPopup")
                    && (popup->property("modal").toBool() || popup->inherits("QQuickMenu")))
                    return item;
            }
        }
        std::stable_sort(children.begin(), children.end(), [](auto a, auto b) { return a->z() < b->z(); });
        for (auto it = children.crbegin(); it != children.crend(); ++it)
            if (auto target = self(self, *it)) return target;
        return inside && !overlay && (item == this || item->acceptedMouseButtons() != Qt::NoButton) ? item : nullptr;
    };
    return hit(hit, window()->contentItem());
}
void NoteCanvas::finishTouchEdit(bool cancel) {
    if (!m_touchEditing) return;
    m_touchEditing = false;
    if (m_touchAction == TouchAction::Draw) {
        if (cancel) cancelStroke(); else finishStroke();
    } else {
        if (cancel) { const auto before = m_before; m_selectionRotation = m_startRotation; restoreSnapshot(before); }
        endEdit();
    }
    m_selecting = false; m_dragging = false; m_marquee = false;
}
void NoteCanvas::cancelTouch() {
    stopContextHold(HoldSource::Touch);
    finishTouchEdit(true);
    m_touchTapTimer.invalidate();
    m_touchPointer = false; m_touchPanned = false; m_touchId = -1;
    m_touchAction = TouchAction::None; m_touchDistance = 0;
    setKeepTouchGrab(false); update();
}
void NoteCanvas::rejectTouchSequence() {
    if (!m_touchPointer && m_touchAction == TouchAction::None) return;
    cancelTouch();
    // A resting palm must lift before it can become a navigation gesture,
    // including after the pen has left proximity and its cooldown has expired.
    m_touchAction = TouchAction::Blocked;
    setKeepTouchGrab(true);
}
bool NoteCanvas::suppressTouchForStylus() const {
    return !m_pensInProximity.isEmpty() || m_stylusDown
        || (m_penTouchCooldown.isValid() && m_penTouchCooldown.elapsed() < 350);
}
void NoteCanvas::startContextHold(const QPointF &position, HoldSource source) {
    stopContextHold(); m_holdPosition = position; m_holdSource = source;
    m_holdElapsed.start(); m_contextHoldTimer.start(); m_shapeTimer.stop(); emit holdChanged();
}
void NoteCanvas::stopContextHold(HoldSource source) {
    if (source != HoldSource::None && source != m_holdSource) return;
    m_holdSource = HoldSource::None;
    m_contextHoldTimer.stop(); m_holdElapsed.invalidate();
    if (m_holdProgress == 0) return;
    m_holdProgress = 0; emit holdChanged();
}
void NoteCanvas::finishInput() {
    setStylusDown(false);
    finishStylus("input-reset"); cancelTouch(); finishStroke(); endEdit();
    m_panning = false; m_selecting = false; m_marquee = false; m_dragging = false;
    m_stylusEraser = false;
    m_rejectedWindowTouch = false;
    setKeepMouseGrab(false); leavePointer();
}
void NoteCanvas::interruptInput() {
    const auto device = m_stylusDevice;
    const auto pressure = m_stylusPressure;
    if (m_touchAction == TouchAction::Draw) finishTouchEdit(false);
    finishInput();
    if (pressure > 0) {
        // Keep the interrupted contact consumed until lift instead of starting a stroke in the new viewport.
        m_stylusDevice = device; m_stylusPressure = pressure; m_stylusAction = StylusAction::Consumed;
        setStylusDown(true);
    }
}
void NoteCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    if (newGeometry.size() != oldGeometry.size()) interruptInput();
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
}
bool NoteCanvas::stylusOnCanvas() const {
    return m_stylusPressure > 0 && m_stylusAction != StylusAction::None && m_stylusAction != StylusAction::Control;
}
void NoteCanvas::finishStylus(const char *reason, bool releaseTool) {
    stopContextHold(HoldSource::Stylus);
    const bool canvas = stylusOnCanvas();
    if (reason && m_stylusPressure > 0) {
        qInfo().noquote() << "[notes.input]" << QTime::currentTime().toString("HH:mm:ss.zzz")
            << "contact=off reason=" + QString::fromLatin1(reason);
        m_pressureLogTimer.invalidate();
    }
    m_stylusPressure = 0; m_stylusAction = StylusAction::None; m_stylusDevice.clear();
    if (releaseTool && !m_stylusToolHeld.isEmpty()) { m_stylusToolHeld.clear(); emit stylusToolHeldChanged(); }
    if (!canvas) return;
    finishStroke(); endEdit();
    m_selecting = false; m_marquee = false; m_dragging = false;
    update();
}
void NoteCanvas::beginTouchAction(const QPointF &position) {
    m_touchPress = m_touchPosition = position; m_touchPanned = false;
    const QPointF point = (position + QPointF(m_offsetX, m_offsetY)) / m_zoom;
    const QString tool = m_drawWithFinger || m_tool == "pointer" ? m_tool : QStringLiteral("pan");
    const bool select = tool == "pointer" || (tool == "pan" && (selectionContains(point) || handleAt(point)));
    updatePointer(position); forceActiveFocus(); m_touchAction = TouchAction::Pan;
    if (select) {
        finishStroke(); endEdit(); beginSelection(point, Qt::NoModifier, tool == "pan");
        m_touchAction = TouchAction::Select; m_touchEditing = true;
    } else if (tool == "pen" || tool == "eraser") {
        clearSelection(); beginStroke(position.x(), position.y(), 1, tool == "eraser");
        m_touchAction = TouchAction::Draw; m_touchEditing = true;
    } else if (tool == "text") m_touchAction = TouchAction::Text;
    if (!stylusOnCanvas()) {
        startContextHold(position, HoldSource::Touch);
    }
}
void NoteCanvas::touchUngrabEvent() { if (m_touchPointer) cancelTouch(); }
bool NoteCanvas::palmTouch(const QTouchEvent *event) const {
    const auto screen = window() ? window()->screen() : QGuiApplication::primaryScreen();
    const qreal palmSize = 24 * (screen ? screen->physicalDotsPerInch() / screen->devicePixelRatio() : 96) / 25.4;
    return event->points().size() > 2 || std::any_of(event->points().cbegin(), event->points().cend(), [&](const QEventPoint &p) {
        return std::max(p.ellipseDiameters().width(), p.ellipseDiameters().height()) >= palmSize;
    });
}
void NoteCanvas::touchEvent(QTouchEvent *event) {
    if (!m_touchAvailable || (event->device() && event->device()->type() != QInputDevice::DeviceType::TouchScreen)) { event->ignore(); return; }
    if (event->type() == QEvent::TouchCancel) { cancelTouch(); event->accept(); return; }
    if (event->points().isEmpty()) { event->accept(); return; }
    if (std::any_of(event->points().cbegin(), event->points().cend(), [](const QEventPoint &p) {
        return !std::isfinite(p.position().x()) || !std::isfinite(p.position().y());
    })) { rejectTouchSequence(); event->accept(); return; }
    if (suppressTouchForStylus() || m_touchAction == TouchAction::Blocked) {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchEnd || qEnvironmentVariableIsSet("PILON_INPUT_DEBUG"))
            qInfo().noquote() << "[notes.input]" << QTime::currentTime().toString("HH:mm:ss.zzz")
                << "source=touch route=canvas palm=rejected reason=pen-or-rejected-sequence"
                << "contacts=" + QString::number(event->points().size());
        cancelTouch();
        if (event->type() != QEvent::TouchEnd) {
            m_touchAction = TouchAction::Blocked;
            setKeepTouchGrab(true);
        }
        event->accept(); return;
    }
    const bool palm = palmTouch(event);
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchEnd || (palm && m_touchAction != TouchAction::Blocked))
        qInfo().noquote() << "[notes.input]" << QTime::currentTime().toString("HH:mm:ss.zzz")
            << "source=touch route=canvas" << (event->type() == QEvent::TouchBegin ? "event=press" : event->type() == QEvent::TouchEnd ? "event=release" : "event=move")
            << "contacts=" + QString::number(event->points().size()) << "tool=" + m_tool
            << "finger-tool=" + (m_drawWithFinger || m_tool == "pointer" ? m_tool : QStringLiteral("pan"))
            << (stylusOnCanvas() ? "pen-contact=on" : "pen-contact=off") << (palm ? "palm=rejected" : "palm=no")
            << "position=" << event->points().first().position() << "start=" << m_touchPress
            << "offset=" << QPointF(m_offsetX, m_offsetY) << "device=" + event->pointingDevice()->name();
    if (qEnvironmentVariableIsSet("PILON_INPUT_DEBUG"))
        qWarning() << "Pilon touch" << event->type() << event->points() << "tool" << m_tool << "palm" << palm
            << "action" << int(m_touchAction) << "previous" << m_touchPosition << "start" << m_touchPress
            << "offset" << QPointF(m_offsetX, m_offsetY) << "threshold" << QGuiApplication::styleHints()->startDragDistance();
    if (palm) { cancelTouch(); m_touchAction = TouchAction::Blocked; event->accept(); return; }
    if (event->type() == QEvent::TouchBegin) {
        const auto &p = event->points().first();
        m_touchPress = m_touchPosition = p.position();
        m_touchId = p.id(); setKeepTouchGrab(true);
        m_touchPointer = true; m_touchPanned = false;
        m_touchAction = TouchAction::Pan;
        if (event->points().size() == 1) beginTouchAction(p.position());
    }
    if (m_touchAction == TouchAction::None || m_touchAction == TouchAction::Blocked || m_touchAction == TouchAction::ContextMenu) {
        if (event->type() == QEvent::TouchEnd) cancelTouch();
        event->accept(); return;
    }
    QList<QEventPoint> active;
    for (const auto &point : event->points()) if (point.state() != QEventPoint::Released) active.append(point);
    if (active.size() == 2) {
        const QPointF center = (active[0].position() + active[1].position()) / 2;
        const qreal distance = QLineF(active[0].position(), active[1].position()).length();
        if (m_touchAction != TouchAction::Pinch) {
            stopContextHold();
            m_touchTapTimer.invalidate();
            finishTouchEdit(true); m_touchAction = TouchAction::Pinch; m_touchPanned = true;
        } else if (m_touchDistance > 1 && distance > 1) {
            moveView(m_zoom * distance / m_touchDistance, m_touchPosition, center);
        }
        m_touchPosition = center; m_touchDistance = distance;
        event->accept(); return;
    }
    if (m_touchAction == TouchAction::Pinch) {
        // After lifting one finger, continue panning from the remaining finger
        // without switching to an item drag or jumping to the old midpoint.
        if (active.size() == 1) {
            m_touchAction = TouchAction::Pan; m_touchId = active.first().id();
            m_touchPress = m_touchPosition = active.first().position();
        } else cancelTouch();
        m_touchDistance = 0; event->accept(); return;
    }
    const auto point = std::find_if(event->points().cbegin(), event->points().cend(), [this](const QEventPoint &p) { return p.id() == m_touchId; });
    if (point == event->points().cend()) { cancelTouch(); event->accept(); return; }
    const auto &p = *point;
    if (!m_touchPanned && QLineF(m_touchPress, p.position()).length() >= QGuiApplication::styleHints()->startDragDistance()) {
        m_touchPanned = true; stopContextHold();
    }
    if (m_touchAction == TouchAction::Draw) {
        addPoint(p.position().x(), p.position().y(), 1);
        if (m_contextHoldTimer.isActive()) m_shapeTimer.stop();
    }
    else if (m_touchPanned && p.position() != m_touchPosition) {
        if (m_touchAction == TouchAction::Select) {
            updatePointer(p.position()); updateSelection(m_pointer);
        } else if (m_touchAction == TouchAction::Pan) moveView(m_zoom, m_touchPosition, p.position());
        m_touchPosition = p.position();
    }
    if (event->type() == QEvent::TouchEnd) {
        // Defer Pan selection until release: dragging an unselected item still
        // pans, while a tap selects it for the next move/resize/rotate gesture.
        const bool tap = !m_touchPanned && !stylusOnCanvas()
            && (m_touchAction == TouchAction::Pan || (m_touchAction == TouchAction::Select && !m_handle));
        const auto position = p.position();
        const auto documentPoint = (position + QPointF(m_offsetX, m_offsetY)) / m_zoom;
        const auto key = tap ? itemAt(documentPoint) : QString{};
        const bool doubleTap = !key.isEmpty() && key == m_touchTapItem && m_touchTapTimer.isValid()
            && m_touchTapTimer.elapsed() <= QGuiApplication::styleHints()->mouseDoubleClickInterval()
            && QLineF(position, m_touchTapPosition).length() <= QGuiApplication::styleHints()->touchDoubleTapDistance();
        finishTouchEdit(false);
        if (tap && m_touchAction == TouchAction::Pan) selectAt(documentPoint.x(), documentPoint.y());
        if (!m_touchPanned && m_touchAction == TouchAction::Text) requestText(position);
        cancelTouch();
        if (doubleTap) {
            for (const auto &value : m_elements) {
                const auto item = value.toMap();
                if (key == "e" + item.value("id").toString() && item.value("type") == "text") {
                    emit editTextRequested(item.value("id").toString()); break;
                }
            }
        } else if (!key.isEmpty()) {
            m_touchTapItem = key; m_touchTapPosition = position; m_touchTapTimer.start();
        }
    }
    event->accept();
}
void NoteCanvas::logStylus(const QTabletEvent *event, const char *route) {
    const bool contact = std::isfinite(event->pressure()) && event->pressure() > 0 && !tabletTipReleased(event);
    const bool eraser = event->pointerType() == QPointingDevice::PointerType::Eraser;
    // Keep idle hover quiet and sample held pressure. Contact/zero transitions
    // and action changes are always logged immediately.
    if (!qEnvironmentVariableIsSet("PILON_INPUT_DEBUG") && event->type() == QEvent::TabletMove
        && contact == m_loggedContact && eraser == m_loggedEraser && m_stylusAction == m_loggedAction
        && m_pressureLogTimer.isValid() && (!contact || m_pressureLogTimer.elapsed() < 250)) return;
    m_pressureLogTimer.start(); m_loggedContact = contact; m_loggedEraser = eraser; m_loggedAction = m_stylusAction;
    const char *actions[] = {"idle", "draw", "erase", "select", "hold", "control", "wait-for-lift", "text"};
    qInfo().noquote() << "[notes.input]" << QTime::currentTime().toString("HH:mm:ss.zzz")
        << (eraser ? "eraser" : "pen") << "pressure=" + QString::number(event->pressure(), 'g', 6)
        << (contact ? "contact=on" : "contact=off") << "action=" + QString::fromLatin1(actions[int(m_stylusAction)])
        << "route=" + QString::fromLatin1(route) << "tool=" + m_tool
        << (event->type() == QEvent::TabletPress ? "event=press" : event->type() == QEvent::TabletRelease ? "event=release" : "event=move")
        << "buttons=" + QString::number(int(event->buttons()))
        << "device=" + event->pointingDevice()->name();
}
bool NoteCanvas::handleTablet(QTabletEvent *event) {
    m_penTouchCooldown.restart();
    rejectTouchSequence();
    trackStylusDevice(event->pointingDevice());
    const bool pressure = std::isfinite(event->pressure()) && event->pressure() > 0;
    const bool release = tabletTipReleased(event);
    const bool press = event->type() == QEvent::TabletPress;
    const bool eraser = event->pointerType() == QPointingDevice::PointerType::Eraser;
    const auto position = mapFromScene(event->position());
    if (!std::isfinite(position.x()) || !std::isfinite(position.y())) {
        setStylusDown(pressure && !release); finishStylus("invalid-position"); event->accept(); return true;
    }
    if (pressure && !release) m_touchTapTimer.invalidate();
    const auto done = [&](bool handled, const char *route) {
        logStylus(event, route);
        if (handled) event->accept();
        return handled;
    };
    // A release/hover from the other end must not stop the current end's contact.
    if (m_stylusDevice && event->pointingDevice() != m_stylusDevice) {
        if (!pressure || event->type() == QEvent::TabletRelease)
            return done(stylusOnCanvas() || ownsInputAt(event->position()), "other-device");
        finishStylus("device-changed");
    }
    setStylusDown(pressure && !release);
    if (m_stylusAction == StylusAction::Control) {
        if (!pressure || release) finishStylus();
        else m_stylusPressure = event->pressure();
        return done(false, "control");
    }
    const bool canvasHit = stylusOnCanvas() || ownsInputAt(event->position());
    QString heldTool;
    if (canvasHit && !eraser) {
        if (event->buttons() & Qt::RightButton) heldTool = m_stylusBindings.value("right").toString();
        if (heldTool.isEmpty() || heldTool == "none") {
            heldTool = event->buttons() & Qt::MiddleButton ? m_stylusBindings.value("middle").toString() : QString{};
        }
        if (heldTool != "pointer" && heldTool != "pen" && heldTool != "eraser" && heldTool != "pan" && heldTool != "text") heldTool.clear();
    }
    setStylusToolHeld(heldTool);
    const auto tool = m_stylusToolHeld.isEmpty() ? m_tool : m_stylusToolHeld;
    const bool drawing = m_stylusToolHeld.isEmpty() ? m_drawEnabled : tool == "pen" || tool == "eraser";
    // Once a gesture has a target, keep it there without scanning the entire
    // QML scene for every pressure sample (or again for Qt's mouse fallback).
    const bool owned = stylusOnCanvas();
    const bool hit = owned || ownsInputAt(event->position());
    if (!pressure || release) {
        // Never let a late release start a new action. A final positive sample
        // may finish an existing stroke, but a zero sample never adds ink.
        if (pressure && owned && (m_stylusAction == StylusAction::Draw || m_stylusAction == StylusAction::Erase))
            addPoint(position.x(), position.y(), event->pressure());
        const bool textTap = owned && m_stylusAction == StylusAction::Text && !m_stylusMoved;
        const auto textPosition = m_stylusPress;
        finishStylus(nullptr, false);
        m_stylusEraser = eraser;
        if (!m_touchPointer && (hit || owned)) updatePointer(position);
        if (press && !hit) { m_stylusAction = StylusAction::Control; m_stylusDevice = event->pointingDevice(); }
        if (textTap) requestText(textPosition);
        return done(hit || owned, hit || owned ? "canvas" : "control");
    }
    if (!owned && event->type() == QEvent::TabletRelease) return done(hit, "late-release");
    if (!owned && !hit) {
        if (press) { m_stylusAction = StylusAction::Control; m_stylusDevice = event->pointingDevice(); m_stylusPressure = event->pressure(); }
        return done(false, "control");
    }
    const bool start = !owned || (press && (!eraser || event->button() == Qt::LeftButton)) || eraser != m_stylusEraser;
    if (start) {
        stopContextHold();
        finishStylus(nullptr, false);
        finishStroke(); endEdit(); m_panning = false;
        m_stylusDevice = event->pointingDevice(); m_stylusPressure = event->pressure(); m_stylusEraser = eraser;
        m_stylusPress = position; m_stylusMoved = false;
        updatePointer(position); forceActiveFocus();
        // The physical eraser always wins, including Pan, Text and side buttons.
        if (eraser || (drawing && (event->button() != Qt::RightButton || !m_stylusToolHeld.isEmpty()))) {
            m_stylusAction = eraser || tool == "eraser" || (m_stylusToolHeld.isEmpty() && m_eraseTool) ? StylusAction::Erase : StylusAction::Draw;
            clearSelection(); beginStroke(position.x(), position.y(), event->pressure(), m_stylusAction == StylusAction::Erase);
        } else if (event->button() == Qt::RightButton && m_stylusToolHeld.isEmpty()) {
            m_stylusAction = StylusAction::Consumed;
            requestContextMenu(position);
        } else if (tool == "pan" && !selectionContains(m_pointer) && !handleAt(m_pointer)) {
            m_stylusAction = StylusAction::Hold;
        } else if (tool == "text") {
            m_stylusAction = StylusAction::Text;
        } else {
            m_stylusAction = StylusAction::Select; beginSelection(m_pointer, event->modifiers(), tool == "pan");
        }
        if (m_stylusAction != StylusAction::Consumed) startContextHold(position, HoldSource::Stylus);
    } else {
        if (!m_stylusMoved && QLineF(position, m_stylusPress).length() >= QGuiApplication::styleHints()->startDragDistance()) {
            m_stylusMoved = true; stopContextHold(HoldSource::Stylus);
        }
        m_stylusPressure = event->pressure(); updatePointer(position);
        switch (m_stylusAction) {
        case StylusAction::Draw: case StylusAction::Erase:
            addPoint(position.x(), position.y(), event->pressure());
            if (m_holdSource == HoldSource::Stylus) m_shapeTimer.stop();
            break;
        case StylusAction::Select: updateSelection(m_pointer); break;
        default: break;
        }
    }
    return done(true, "canvas");
}
bool NoteCanvas::handleStylusMouse(QMouseEvent *event) {
    // Qt's copies follow tablet samples and have no pressure information. A
    // device delivered mouse stream is also possible after digitizer hotplug;
    // handle that contact with constant pressure until tablet samples resume.
    if (event->source() == Qt::MouseEventSynthesizedByQt
        || (event->pointingDevice() == m_tabletSampleDevice && m_tabletSampleTimer.isValid() && m_tabletSampleTimer.elapsed() < 150)) return false;
    const auto type = event->type() == QEvent::MouseButtonRelease ? QEvent::TabletRelease
        : event->type() == QEvent::MouseMove ? QEvent::TabletMove : QEvent::TabletPress;
    const qreal pressure = event->buttons() & Qt::LeftButton ? 1 : 0;
    QTabletEvent tablet(type, event->pointingDevice(), event->position(), event->globalPosition(), pressure,
        0, 0, 0, 0, 0, event->modifiers(), event->button(), event->buttons());
    if (!handleTablet(&tablet)) return false;
    event->accept(); return true;
}
bool NoteCanvas::eventFilter(QObject *object, QEvent *event) {
    if (event->type() == QEvent::TabletEnterProximity) {
        const auto device = event->isPointerEvent() ? static_cast<QTabletEvent *>(event)->pointingDevice() : nullptr;
        trackStylusDevice(device); m_pensInProximity.insert(device);
        m_penTouchCooldown.restart(); rejectTouchSequence(); return false;
    }
    if (event->type() == QEvent::TabletLeaveProximity) {
        const auto device = event->isPointerEvent() ? static_cast<QTabletEvent *>(event)->pointingDevice() : nullptr;
        if (device) m_pensInProximity.remove(device);
        else m_pensInProximity.clear();
        m_penTouchCooldown.restart(); rejectTouchSequence();
        if (!device || !m_stylusDevice || device == m_stylusDevice) {
            setStylusDown(false); finishStylus("proximity-leave"); m_stylusEraser = false;
        }
        if (m_pensInProximity.isEmpty()) leavePointer();
        return false;
    }
    if (event->type() == QEvent::TabletPress || event->type() == QEvent::TabletMove || event->type() == QEvent::TabletRelease) {
        auto tablet = static_cast<QTabletEvent *>(event);
        if (object == window() && isVisible() && isEnabled()) {
            m_tabletSampleDevice = tablet->pointingDevice(); m_tabletSampleTimer.start(); return handleTablet(tablet);
        }
        // Qt can target another window on lift (for example an open popup).
        if (object->isWindowType() && m_stylusDevice == tablet->pointingDevice()
            && (!(tablet->pressure() > 0) || !std::isfinite(tablet->pressure()) || tabletTipReleased(tablet))) {
            setStylusDown(false); finishStylus(); logStylus(tablet, "outside-window");
        }
        return false;
    }
    if (object != window() || !isVisible() || !isEnabled()) return false;
    if (event->type() == QEvent::WindowActivate || event->type() == QEvent::TouchBegin) refreshTouchSupport();
    if (event->type() == QEvent::WindowDeactivate) { finishInput(); return false; }
    if (event->type() == QEvent::TouchCancel) { m_rejectedWindowTouch = false; cancelTouch(); return false; }
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
        const auto touch = static_cast<QTouchEvent *>(event);
        if (event->type() == QEvent::TouchBegin) m_rejectedWindowTouch = false;
        const bool invalid = std::any_of(touch->points().cbegin(), touch->points().cend(), [](const QEventPoint &point) {
            return !std::isfinite(point.position().x()) || !std::isfinite(point.position().y());
        });
        if (m_rejectedWindowTouch || suppressTouchForStylus() || palmTouch(touch) || invalid) {
            // A palm can land on the toolbars or cross the paper's edges. Reject
            // it before Qt delivers a press to any control in the Notes window.
            // If classification changes after a press, release Qt's control
            // grabs as well as the canvas's own gesture state.
            if (!m_rejectedWindowTouch && event->type() != QEvent::TouchBegin) {
                QTouchEvent cancel(QEvent::TouchCancel, touch->pointingDevice(), touch->modifiers());
                QCoreApplication::sendEvent(window(), &cancel);
            }
            rejectTouchSequence(); m_rejectedWindowTouch = event->type() != QEvent::TouchEnd;
            if (!m_rejectedWindowTouch) cancelTouch();
            touch->accept(); return true;
        }
    }
    if (event->type() == QEvent::NativeGesture) {
        const auto gesture = static_cast<QNativeGestureEvent *>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture && ownsInputAt(gesture->position())) {
            const auto p = mapFromScene(gesture->position()); zoomAt(1 + gesture->value(), p.x(), p.y());
            event->accept(); return true;
        }
    }
    if (event->type() == QEvent::Wheel) {
        auto wheel = static_cast<QWheelEvent *>(event);
        if (m_stylusDown && contains(mapFromScene(wheel->position()))) { wheel->accept(); return true; }
        if (ownsInputAt(wheel->position())) {
            QWheelEvent local(mapFromScene(wheel->position()), wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted());
            handleWheel(&local); wheel->accept(); return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick) {
        auto mouse = static_cast<QMouseEvent *>(event);
        if (touchMouse(mouse) || (mouse->source() != Qt::MouseEventNotSynthesized && !stylusMouse(mouse)
            && (suppressTouchForStylus() || m_touchAction == TouchAction::Blocked))) {
            // A rejected palm must not return through platform/Qt mouse
            // synthesis and start a second, unguarded navigation gesture.
            if (ownsInputAt(mouse->position()) || m_panning || m_touchAction == TouchAction::Blocked) {
                mouse->accept(); return true;
            }
        }
        if (stylusMouse(mouse) && handleStylusMouse(mouse)) return true;
        if (stylusOnCanvas()) { mouse->accept(); return true; }
        if (stylusMouse(mouse)) {
            // Let Qt finish control grabs, including a release over the paper.
            // The canvas's mouse handlers cannot turn these into pen actions.
            if (event->type() != QEvent::MouseMove || qEnvironmentVariableIsSet("PILON_INPUT_DEBUG"))
                qInfo().noquote() << "[notes.input]" << QTime::currentTime().toString("HH:mm:ss.zzz")
                    << "source=mouse pressure=unavailable canvas-action=none device=" + mouse->pointingDevice()->name();
            return false;
        }
        const bool hit = ownsInputAt(mouse->position());
        if (hit && m_touchPointer) { mouse->accept(); return true; }
        const auto position = mapFromScene(mouse->position());
        if (hit) updatePointer(position);
        if ((event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::MiddleButton && hit) || m_panning) {
            QMouseEvent local(event->type(), position, mouse->globalPosition(), mouse->button(), mouse->buttons(), mouse->modifiers());
            if (event->type() == QEvent::MouseButtonPress) mousePressEvent(&local);
            else if (event->type() == QEvent::MouseButtonRelease) mouseReleaseEvent(&local);
            else mouseMoveEvent(&local);
            mouse->accept(); return true;
        }
    }
    return false;
}
void NoteCanvas::setDocumentId(const QString &id) {
    if (m_documentId == id) return;
    setStylusDown(false);
    stopContextHold();
    m_touchTapTimer.invalidate();
    m_shapeTimer.stop(); m_recognizedStroke.clear();
    m_drawing = false; m_editing = false; m_selecting = false; m_points.clear(); m_liveTiles.clear(); m_liveBounds = {}; m_before.clear(); m_undo.clear(); m_redo.clear(); clearSelection();
    m_stylusPressure = 0; m_stylusAction = StylusAction::None; m_stylusDevice.clear(); m_stylusEraser = false;
    if (!m_stylusToolHeld.isEmpty()) { m_stylusToolHeld.clear(); emit stylusToolHeldChanged(); }
    m_touchPointer = false; m_touchPanned = false; m_touchId = -1;
    m_touchEditing = false; m_touchAction = TouchAction::None; m_touchDistance = 0;
    m_zoom = 1; emit paperScaleChanged(); emit documentHeightChanged();
    m_panning = false; m_marquee = false; m_dragging = false; setKeepTouchGrab(false); setKeepMouseGrab(false);
    m_documentId = id; emit documentChanged(); emit historyChanged(); update();
}
void NoteCanvas::setStrokes(const QVariantList &strokes) {
    if (m_strokes == strokes) return;
    m_shapeTimer.stop(); m_recognizedStroke.clear();
    m_strokes = strokes; if (!m_editing) { m_undo.clear(); m_redo.clear(); } m_points.clear(); m_liveTiles.clear(); m_liveBounds = {}; m_drawing = false;
    emit historyChanged(); updateSelectionFrame(); publish();
}
void NoteCanvas::setElements(const QVariantList &elements) {
    if (m_elements == elements) return;
    m_elements = elements; invalidateContent(); updateSelectionFrame(); emit elementsChanged(); emit documentHeightChanged(); update();
}
void NoteCanvas::setPaper(const QVariantMap &paper) {
    const auto normalized = NoteDocument::normalizedPaper(paper);
    if (normalized == m_paper) return;
    m_paper = normalized; emit paperChanged(); emit documentHeightChanged(); update();
}
void NoteCanvas::setOffsetX(qreal value) { if (m_offsetX != value) { m_pointer.rx() += (value - m_offsetX) / m_zoom; m_offsetX = value; emit pointerChanged(); emit viewChanged(); update(); } }
void NoteCanvas::setOffsetY(qreal value) { if (m_offsetY != value) { m_pointer.ry() += (value - m_offsetY) / m_zoom; m_offsetY = value; emit pointerChanged(); emit viewChanged(); update(); } }
void NoteCanvas::setShowInk(bool show) { if (m_showInk != show) { m_showInk = show; m_tiles.clear(); emit viewChanged(); update(); } }
void NoteCanvas::setPenColour(const QColor &colour) { if (colour.isValid() && colour != m_penColour) { m_penColour = colour; emit toolChanged(); } }
void NoteCanvas::setPenWidth(qreal width) { width = std::clamp(width, .5, 24.); if (m_penWidth != width) { m_penWidth = width; emit toolChanged(); } }
void NoteCanvas::setRecognizeShapes(bool enabled) { if (m_recognize != enabled) { m_recognize = enabled; emit toolChanged(); } }
void NoteCanvas::beginStroke(qreal x, qreal y, qreal pressure, bool erase) {
    if (m_documentId.isEmpty()) return;
    finishStroke(); beginEdit(); m_drawing = true; m_erasing = erase; m_hasErasePoint = false; m_points.clear(); m_liveTiles.clear(); m_liveBounds = {};
    m_recognizedStroke.clear(); m_shapeTimer.stop(); m_holdAnchor = QPointF(x + m_offsetX, y + m_offsetY) / m_zoom;
    addPoint(x, y, pressure);
}
void NoteCanvas::addPoint(qreal x, qreal y, qreal pressure) {
    if (!m_drawing || !std::isfinite(x) || !std::isfinite(y)) return;
    const QPointF p((x + m_offsetX) / paperScale(), (y + m_offsetY) / paperScale());
    const QSizeF previousSize(documentWidth(), documentHeight());
    if (m_erasing) {
        const qreal radius = m_eraserSize / 2;
        const QPointF start = m_hasErasePoint ? m_lastErase : p;
        const int steps = std::max(1, int(std::ceil(QLineF(start, p).length() / std::max(1., radius / 2))));
        for (int i = 1; i <= steps; ++i) {
            const QPointF center = start + (p - start) * (qreal(i) / steps);
            QVariantList remaining;
            for (const auto &value : m_strokes) {
                const auto stroke = value.toMap();
                if (m_eraserMode == "area") remaining += NoteDocument::eraseArea(stroke, center, radius);
                else if (!NoteDocument::strokeHit(stroke, center, radius)) remaining.append(stroke);
            }
            m_strokes = remaining;
        }
        m_lastErase = p; m_hasErasePoint = true;
    } else {
        if (!m_recognizedStroke.isEmpty()) {
            adjustRecognizedStroke(p); return;
        }
        if (m_recognize && (!m_shapeTimer.isActive() || QLineF(p, m_holdAnchor).length() > 3)) {
            m_holdAnchor = p; m_shapeTimer.start();
        }
        if (!m_points.isEmpty()) {
            const auto previous = m_points.last().toMap();
            if (QLineF(QPointF(previous.value("x").toDouble(), previous.value("y").toDouble()), p).length() < .35) return;
        }
        m_liveBounds = m_liveBounds.united(QRectF(p, QSizeF(.1, .1)));
        m_points.append(QVariantMap{{"x", p.x()}, {"y", p.y()}, {"pressure", m_pressureSensitive && std::isfinite(pressure) ? std::clamp(pressure, .05, 1.) : 1.}});
    }
    if (m_erasing) invalidateContent();
    if (previousSize != QSizeF(documentWidth(), documentHeight())) emit documentHeightChanged();
    if (m_erasing) update();
    else {
        // Only the tail changes when the next sample extends a smoothed curve.
        // Keep earlier live tiles, including their pressure taper, intact.
        QRectF tail(p, QSizeF(.1, .1));
        for (int i = std::max(0, int(m_points.size()) - 4); i < m_points.size(); ++i) {
            const auto point = m_points[i].toMap();
            tail = tail.united(QRectF(QPointF(point.value("x").toDouble(), point.value("y").toDouble()), QSizeF(.1, .1)));
        }
        const qreal scale = paperScale(), margin = std::max(.5, m_penWidth) / 2 + 2 / scale;
        tail.adjust(-margin, -margin, margin, margin);
        for (const auto &key : m_liveTiles.keys()) {
            const QRectF tile(m_tileOrigin + QPointF(key.first * m_tileSize.width(), key.second * m_tileSize.height()), m_tileSize);
            if (tile.intersects(tail)) m_liveTiles.remove(key);
        }
        const QRectF bounds(tail.topLeft() * scale - QPointF(m_offsetX, m_offsetY), tail.size() * scale);
        const auto dirty = bounds.intersected(boundingRect()).toAlignedRect();
        if (!dirty.isEmpty()) update(dirty);
    }
}
void NoteCanvas::finishStroke() {
    if (!m_drawing) return;
    m_drawing = false; m_shapeTimer.stop();
    if (!m_erasing && !m_points.isEmpty()) {
        auto stroke = NoteDocument::normalizedStroke({{"type", "freehand"}, {"points", m_points},
            {"width", m_penWidth}, {"colour", m_penColour.name(QColor::HexArgb)}});
        if (!m_recognizedStroke.isEmpty()) stroke = m_recognizedStroke;
        if (!stroke.isEmpty()) m_strokes.append(stroke);
    }
    m_points.clear(); m_liveTiles.clear(); m_liveBounds = {}; m_recognizedStroke.clear();
    endEdit(); publish(); update();

}
void NoteCanvas::recognizeHeldStroke() {
    if (!m_drawing || m_erasing || !m_recognize || !m_recognizedStroke.isEmpty()) return;
    auto stroke = NoteDocument::recognizeShape(NoteDocument::normalizedStroke({{"type", "freehand"},
        {"points", m_points}, {"width", m_penWidth}, {"colour", m_penColour.name(QColor::HexArgb)}}));
    // Join touching pieces, including reversed sides, before recognizing a
    // closed shape. A pen lift between its sides need not break recognition.
    if (stroke.value("type") == "freehand" || stroke.value("type") == "line") {
        auto joined = m_points;
        const auto point = [](const QVariant &v) { const auto p = v.toMap(); return QPointF(p.value("x").toDouble(), p.value("y").toDouble()); };
        for (int count = 1; count <= std::min(4, int(m_strokes.size())); ++count) {
            const auto previous = m_strokes[m_strokes.size()-count].toMap();
            if ((previous.value("type") != "freehand" && previous.value("type") != "line")
                || previous.value("colour") != stroke.value("colour") || previous.value("width") != stroke.value("width")) break;
            auto part = previous.value("points").toList(); if (part.isEmpty() || joined.isEmpty()) break;
            for (auto &v : part) { const auto p = NoteDocument::strokeTransform(previous).map(point(v)); auto vertex = v.toMap(); vertex["x"] = p.x(); vertex["y"] = p.y(); v = vertex; }
            const qreal distances[] = {QLineF(point(part.last()), point(joined.first())).length(), QLineF(point(part.first()), point(joined.first())).length(),
                QLineF(point(part.first()), point(joined.last())).length(), QLineF(point(part.last()), point(joined.last())).length()};
            const int end = int(std::min_element(std::begin(distances), std::end(distances)) - std::begin(distances));
            const auto bounds = NoteDocument::strokeBounds(previous).united(NoteDocument::strokeBounds(stroke));
            if (distances[end] > std::max(8., std::hypot(bounds.width(), bounds.height()) * .07)) break;
            if (end == 1 || end == 3) std::reverse(part.begin(), part.end());
            joined = end < 2 ? part + joined : joined + part;
            auto candidate = NoteDocument::recognizeShape(NoteDocument::normalizedStroke({{"type", "freehand"}, {"points", joined},
                {"width", m_penWidth}, {"colour", m_penColour.name(QColor::HexArgb)}}));
            if (candidate.value("type") != "freehand" && candidate.value("type") != "line") {
                stroke = candidate; m_strokes.resize(m_strokes.size()-count); invalidateContent(); break;
            }
        }
    }
    if (stroke.isEmpty() || stroke.value("type") == "freehand") return;
    m_recognizedStroke = stroke; m_snapOriginal = stroke;
    if (stroke.value("type") == "line") adjustRecognizedStroke(m_holdAnchor);
    emit shapeRecognized(); update();
}
void NoteCanvas::adjustRecognizedStroke(const QPointF &p) {
    if (m_snapOriginal.value("type") == "line") {
        const auto start = NoteDocument::strokePath(m_snapOriginal).pointAtPercent(0);
        auto delta = p - start; const qreal angle = std::atan2(delta.y(), delta.x());
        const qreal snapped = std::round(angle / (std::numbers::pi / 4)) * (std::numbers::pi / 4);
        if (std::abs(angle - snapped) < std::numbers::pi / 24) {
            const QPointF axis(std::cos(snapped), std::sin(snapped)); delta = axis * QPointF::dotProduct(delta, axis);
        }
        auto line = m_snapOriginal; line.remove("transform");
        line["points"] = QVariantList{QVariantMap{{"x", start.x()}, {"y", start.y()}, {"pressure", 1.}},
            QVariantMap{{"x", start.x()+delta.x()}, {"y", start.y()+delta.y()}, {"pressure", 1.}}};
        m_recognizedStroke = NoteDocument::normalizedStroke(line);
    } else {
        if (QLineF(p, m_holdAnchor).length() < 5) return;
        const auto center = NoteDocument::strokeBounds(m_snapOriginal).center();
        const auto a = m_holdAnchor - center, b = p - center;
        const qreal scale = std::clamp(QLineF(center, p).length() / std::max(1., QLineF(center, m_holdAnchor).length()), .1, 20.);
        qreal angle = (std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x())) * 180 / std::numbers::pi;
        const qreal original = m_snapOriginal.value("rotation").toDouble();
        const qreal snapped = std::round((original + angle) / 45.) * 45.;
        if (std::abs(original + angle - snapped) <= 5.) angle = snapped - original;
        QTransform transform; transform.translate(center.x(), center.y()); transform.rotate(angle); transform.scale(scale, scale); transform.translate(-center.x(), -center.y());
        m_recognizedStroke = NoteDocument::transformedStroke(m_snapOriginal, transform); m_recognizedStroke["rotation"] = m_snapOriginal.value("rotation").toDouble() + angle;
    }
    m_liveBounds = NoteDocument::strokeBounds(m_recognizedStroke); emit documentHeightChanged(); update();
}
void NoteCanvas::cancelStroke() {
    m_shapeTimer.stop(); m_recognizedStroke.clear();
    if (!m_drawing) return;
    m_drawing = false; restoreSnapshot(m_before); m_editing = false; m_before.clear(); m_points.clear(); m_liveTiles.clear(); m_liveBounds = {}; if (m_notes) m_notes->setWriting(false); emit documentHeightChanged(); update();
}
void NoteCanvas::publish() { invalidateContent(); emit strokesChanged(); emit documentHeightChanged(); update(); }
QVariantMap NoteCanvas::snapshot() const { return {{"strokes", m_strokes}, {"elements", m_elements}}; }
void NoteCanvas::beginEdit() { if (!m_editing) { m_before = snapshot(); m_editing = true; if (m_notes) m_notes->setWriting(true); } }
void NoteCanvas::endEdit() {
    if (!m_editing) return;
    // An external edit/undo or a lost mouse grab may end the transaction while
    // a finger is still down. Never apply the next touch update to an empty snapshot.
    if (m_touchEditing) { m_touchEditing = false; m_touchAction = TouchAction::Blocked; m_selecting = false; m_dragging = false; m_marquee = false; }
    m_editing = false;
    if (snapshot() != m_before) { m_undo.append(m_before); if (m_undo.size() > 100) m_undo.removeFirst(); m_redo.clear(); emit historyChanged(); }
    if (m_strokes != m_before.value("strokes").toList()) emit strokesChanged();
    m_before.clear(); if (m_notes) m_notes->setWriting(false);
}
void NoteCanvas::restoreSnapshot(const QVariantMap &state) {
    m_strokes = state.value("strokes").toList(); m_elements = state.value("elements").toList();
    publish(); emit elementsChanged(); emit elementsEdited(); emit selectionChanged(); update();
}
void NoteCanvas::undo() {
    finishStroke(); endEdit(); if (m_undo.isEmpty()) return;
    m_redo.append(snapshot()); const auto state = m_undo.takeLast(); restoreSnapshot(state); clearSelection(); emit historyChanged();
}
void NoteCanvas::redo() {
    finishStroke(); endEdit(); if (m_redo.isEmpty()) return;
    m_undo.append(snapshot()); const auto state = m_redo.takeLast(); restoreSnapshot(state); clearSelection(); emit historyChanged();
}
QString NoteCanvas::itemAt(const QPointF &point) const {
    for (int i = m_elements.size() - 1; i >= 0; --i) {
        const auto item = m_elements[i].toMap(); if (item.value("type") != "text") continue;
        if (QRectF(0, 0, item.value("width").toDouble(), item.value("height").toDouble()).contains(NoteDocument::elementTransform(item).inverted().map(point))) return "e" + item.value("id").toString();
    }
    for (int i = m_strokes.size() - 1; i >= 0; --i) {
        const auto stroke = m_strokes[i].toMap();
        if ((stroke.value("type") != "freehand" && NoteDocument::strokeBounds(stroke).contains(point)) || NoteDocument::strokeHit(stroke, point, 8)) return "s" + QString::number(i);
    }
    for (int i = m_elements.size() - 1; i >= 0; --i) {
        const auto item = m_elements[i].toMap(); if (item.value("type") == "text") continue;
        if (QRectF(0, 0, item.value("width").toDouble(), item.value("height").toDouble()).contains(NoteDocument::elementTransform(item).inverted().map(point))) return "e" + item.value("id").toString();
    }
    return {};
}
QRectF NoteCanvas::itemBounds(const QString &key) const {
    if (key.startsWith('s')) { const int i = key.mid(1).toInt(); return i >= 0 && i < m_strokes.size() ? NoteDocument::strokeBounds(m_strokes[i].toMap()) : QRectF{}; }
    for (const auto &value : m_elements) if (key == "e" + value.toMap().value("id").toString()) return NoteDocument::elementBounds(value.toMap());
    return {};
}
QRectF NoteCanvas::selectionBounds() const { QRectF box; for (const auto &key : m_selection) box = box.united(itemBounds(key)); return box; }
void NoteCanvas::updateSelectionFrame() {
    m_frame = {}; m_frameTransform = {};
    if (m_selection != m_frameSelection) { m_selectionRotation = 0; m_frameSelection = m_selection; }
    if (m_selection.size() == 1) {
        const auto key = m_selection.first();
        if (key.startsWith('e')) for (const auto &value : m_elements) {
            const auto item = value.toMap(); if (key != "e" + item.value("id").toString()) continue;
            m_frame = QRectF(0, 0, item.value("width").toDouble(), item.value("height").toDouble());
            m_frameTransform = NoteDocument::elementTransform(item); m_selectionRotation = item.value("rotation").toDouble(); return;
        }
        else { const int i = key.mid(1).toInt(); if (i >= 0 && i < m_strokes.size()) m_selectionRotation = m_strokes[i].toMap().value("rotation").toDouble(); }
    }
    m_frameTransform.rotate(m_selectionRotation); const auto inverse = m_frameTransform.inverted();
    // Preserve the user's angle. Moving/resizing never estimates a new angle from the outline.
    for (const auto &key : m_selection) {
        if (key.startsWith('s')) {
            const int i = key.mid(1).toInt(); if (i < 0 || i >= m_strokes.size()) continue;
            const auto stroke = m_strokes[i].toMap(); const qreal radius = stroke.value("width").toDouble()/2;
            m_frame = m_frame.united(inverse.map(NoteDocument::strokePath(stroke)).boundingRect().adjusted(-radius, -radius, radius, radius));
        } else for (const auto &value : m_elements) {
            const auto item = value.toMap(); if (key == "e" + item.value("id").toString())
                m_frame = m_frame.united((NoteDocument::elementTransform(item)*inverse).mapRect(QRectF(0, 0, item.value("width").toDouble(), item.value("height").toDouble())));
        }
    }
}
QPolygonF NoteCanvas::selectionPolygon() const { return m_frameTransform.map(QPolygonF(m_frame)); }
QList<QPointF> NoteCanvas::selectionHandles() const {
    const auto b = m_frame;
    QList<QPointF> handles{b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft(), {b.center().x(), b.top()-24},
        {b.center().x(), b.top()}, {b.right(), b.center().y()}, {b.center().x(), b.bottom()}, {b.left(), b.center().y()}};
    for (auto &p : handles) p = m_frameTransform.map(p); return handles;
}
int NoteCanvas::handleAt(const QPointF &p) const {
    if (m_selection.isEmpty() || (m_tool != "pointer" && m_tool != "pan" && !m_touchPointer)) return 0;
    const auto handles = selectionHandles();
    for (int i = 0; i < handles.size(); ++i) if (QLineF(p, handles[i]).length() <= 9) return i + 1;
    return 0;
}
void NoteCanvas::selectAt(qreal x, qreal y, int modifiers) {
    const auto key = itemAt(QPointF(x, y));
    if (!(modifiers & (Qt::ControlModifier | Qt::ShiftModifier))) m_selection.clear();
    if (!key.isEmpty()) { if (m_selection.contains(key) && modifiers & Qt::ControlModifier) m_selection.removeAll(key); else if (!m_selection.contains(key)) m_selection.append(key); }
    emit selectionChanged(); update();
}
void NoteCanvas::clearSelection() { m_selection.clear(); emit selectionChanged(); update(); }
void NoteCanvas::setSelectedIds(const QStringList &ids) {
    QSet<QString> available;
    for (qsizetype i = 0; i < m_strokes.size(); ++i) available.insert("s" + QString::number(i));
    for (const auto &value : m_elements) {
        const auto id = value.toMap().value("id").toString();
        if (!id.isEmpty()) available.insert("e" + id);
    }
    QStringList selected;
    for (const auto &id : ids) if (available.remove(id)) selected.append(id);
    if (selected == m_selection) return;
    interruptInput();
    m_selection = selected; emit selectionChanged(); update();
}
void NoteCanvas::selectAll() {
    m_selection.clear();
    for (int i = 0; i < m_strokes.size(); ++i) m_selection.append("s" + QString::number(i));
    for (const auto &value : m_elements) m_selection.append("e" + value.toMap().value("id").toString());
    emit selectionChanged(); update();
}
void NoteCanvas::applyTransform(const QVariantMap &state, const QTransform &transform, qreal scaleX, qreal scaleY, qreal degrees) {
    auto strokes = state.value("strokes").toList(), elements = state.value("elements").toList();
    for (int i = 0; i < strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) {
        auto stroke = NoteDocument::transformedStroke(strokes[i].toMap(), transform);
        stroke["rotation"] = strokes[i].toMap().value("rotation").toDouble() + degrees; strokes[i] = stroke;
    }
    for (auto &value : elements) {
        auto item = value.toMap(); if (!m_selection.contains("e" + item.value("id").toString())) continue;
        const qreal oldWidth = item.value("width").toDouble(), oldHeight = item.value("height").toDouble();
        const auto mapped = NoteDocument::elementTransform(item) * transform;
        const auto origin = mapped.map(QPointF{}), u = mapped.map(QPointF(oldWidth, 0)) - origin, v = mapped.map(QPointF(0, oldHeight)) - origin;
        const qreal width = std::hypot(u.x(), u.y());
        if (width < .001) continue;
        const qreal height = (u.x()*v.y() - u.y()*v.x()) / width;
        if (height < .001) continue;
        const qreal shear = QPointF::dotProduct(u, v) / (width*height);
        qreal rotation = std::atan2(u.y(), u.x()) * 180 / std::numbers::pi;
        const qreal expected = item.value("rotation").toDouble() + degrees;
        if (std::abs(std::remainder(rotation - expected, 360.)) < 1e-8) rotation = expected;
        const auto center = mapped.map(QPointF(oldWidth/2, oldHeight/2));
        item["x"] = center.x() - width/2; item["y"] = center.y() - height/2;
        item["width"] = qFuzzyCompare(width, oldWidth*scaleX) ? oldWidth*scaleX : width;
        item["height"] = qFuzzyCompare(height, oldHeight*scaleY) ? oldHeight*scaleY : height;
        item["rotation"] = rotation;
        if (std::abs(shear) > 1e-8) item["shear"] = shear; else item.remove("shear");
        value = item;
    }
    const bool movedElements = m_elements != elements;
    m_strokes = strokes; m_elements = elements; invalidateContent(); emit documentHeightChanged();
    if (movedElements) { emit elementsChanged(); emit elementsEdited(); }
    m_selectionRotation = (m_selecting ? m_startRotation : m_selectionRotation) + degrees;
    emit selectionChanged(); update();
}
void NoteCanvas::moveSelection(qreal dx, qreal dy) { beginEdit(); applyTransform(snapshot(), QTransform::fromTranslate(dx, dy), 1, 1, 0); endEdit(); }
void NoteCanvas::transformSelection(qreal scale, qreal degrees) {
    if (!std::isfinite(scale) || !std::isfinite(degrees)) return;
    const qreal absolute = m_selectionRotation + degrees;
    const qreal snapped = std::round(absolute / 45.) * 45.;
    if (std::abs(absolute - snapped) <= 5.) degrees = snapped - m_selectionRotation;
    scale = std::clamp(scale, .05, 20.); const auto center = selectionBounds().center();
    QTransform transform; transform.translate(center.x(), center.y()); transform.rotate(degrees); transform.scale(scale, scale); transform.translate(-center.x(), -center.y());
    beginEdit(); applyTransform(snapshot(), transform, scale, scale, degrees); endEdit();
}
void NoteCanvas::deleteSelection() {
    if (m_selection.isEmpty()) return;
    beginEdit();
    for (int i = m_strokes.size() - 1; i >= 0; --i) if (m_selection.contains("s" + QString::number(i))) m_strokes.removeAt(i);
    m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(), [&](const QVariant &v) { return m_selection.contains("e" + v.toMap().value("id").toString()); }), m_elements.end());
    publish(); emit elementsChanged(); emit elementsEdited(); clearSelection(); endEdit();
}
void NoteCanvas::copySelection(bool cut) {
    if (m_selection.isEmpty()) return;
    QVariantList strokes, elements;
    for (int i = 0; i < m_strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) strokes.append(m_strokes[i]);
    for (const auto &value : m_elements) if (m_selection.contains("e" + value.toMap().value("id").toString())) elements.append(value);
    const auto box = selectionBounds();
    auto mime = new QMimeData;
    mime->setData("application/x-pilon-note-items", QJsonDocument::fromVariant(QVariantMap{{"strokes", strokes}, {"elements", elements}, {"x", box.x()}, {"y", box.y()}}).toJson(QJsonDocument::Compact));
    QGuiApplication::clipboard()->setMimeData(mime); if (cut) deleteSelection();
}
bool NoteCanvas::hasSelectedInk() const {
    for (int i = 0; i < m_strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) return true;
    return false;
}
QColor NoteCanvas::selectedInkColour() const {
    for (int i = 0; i < m_strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) return QColor(m_strokes[i].toMap().value("colour").toString());
    return m_penColour;
}
qreal NoteCanvas::selectedInkWidth() const {
    for (int i = 0; i < m_strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) return m_strokes[i].toMap().value("width").toDouble();
    return m_penWidth;
}
void NoteCanvas::styleSelectedInk(const QColor &colour, qreal width, bool changeColour, bool changeWidth) {
    if (!hasSelectedInk() || !colour.isValid() || !std::isfinite(width)) return;
    finishStroke(); beginEdit();
    for (int i = 0; i < m_strokes.size(); ++i) if (m_selection.contains("s" + QString::number(i))) {
        auto stroke = m_strokes[i].toMap();
        if (changeColour) stroke["colour"] = colour.name(QColor::HexArgb);
        if (changeWidth) stroke["width"] = std::clamp(width, .5, 24.);
        m_strokes[i] = NoteDocument::normalizedStroke(stroke);
    }
    publish(); emit selectionChanged(); endEdit(); update();
}
void NoteCanvas::invalidateContent() {
    const auto invalidate = [&](const QRectF &bounds) {
        for (const auto &key : m_tiles.keys()) if (bounds.intersects(QRectF(m_tileOrigin + QPointF(key.first*m_tileSize.width(), key.second*m_tileSize.height()), m_tileSize))) m_tiles.remove(key);
    };
    const auto changed = [&](const QVariantList &before, const QVariantList &after, bool ink) {
        for (int i = 0; i < std::max(before.size(), after.size()); ++i) {
            if (i < before.size() && i < after.size() && before[i] == after[i]) continue;
            if (i < before.size()) invalidate(ink ? NoteDocument::strokeBounds(before[i].toMap()) : NoteDocument::elementBounds(before[i].toMap()));
            if (i < after.size()) invalidate(ink ? NoteDocument::strokeBounds(after[i].toMap()) : NoteDocument::elementBounds(after[i].toMap()));
        }
    };
    changed(m_cachedStrokes, m_strokes, true); changed(m_cachedElements, m_elements, false);
    m_cachedStrokes = m_strokes; m_cachedElements = m_elements;
    m_contentBounds = {};
    for (const auto &value : m_strokes) m_contentBounds = m_contentBounds.united(NoteDocument::strokeBounds(value.toMap()));
    for (const auto &value : m_elements) m_contentBounds = m_contentBounds.united(NoteDocument::elementBounds(value.toMap()));
}
void NoteCanvas::paintContent(QPainter &painter, const QRectF &visible) {
    for (const auto &value : m_elements) {
        const auto element = value.toMap();
        if (element.value("type") == "text") continue;
        const QRectF box = NoteDocument::elementBounds(element);
        if (!visible.intersects(box)) continue;
        const auto path = element.value("url").toUrl().toLocalFile();
        auto image = m_images.object(path);
        if (!image) {
            QImageReader reader(path);
            if (reader.size().width() > 4096 || reader.size().height() > 4096) reader.setScaledSize(reader.size().scaled(4096, 4096, Qt::KeepAspectRatio));
            auto loaded = reader.read(); if (loaded.isNull()) continue;
            image = new QImage(loaded); m_images.insert(path, image, std::max(1, int(loaded.sizeInBytes() / 1024)));
            image = m_images.object(path); if (!image) continue;
        }
        painter.save(); painter.setTransform(NoteDocument::elementTransform(element), true);
        painter.drawImage(QRectF(0, 0, element.value("width").toDouble(), element.value("height").toDouble()), *image); painter.restore();
    }
    if (!m_showInk) return;
    for (const auto &value : m_strokes) {
        const auto stroke = value.toMap();
        if (visible.intersects(NoteDocument::strokeBounds(stroke))) NoteDocument::paintStroke(painter, stroke);
    }
}
void NoteCanvas::paint(QPainter *painter) {
    const qreal scale = paperScale();
    painter->scale(scale, scale);
    const QRectF viewport(0, 0, width() / scale, height() / scale);
    NoteDocument::paintPaper(*painter, viewport, m_paper, QPointF(m_offsetX / scale, m_offsetY / scale));
    painter->setRenderHint(QPainter::Antialiasing);
    painter->translate(-m_offsetX / scale, -m_offsetY / scale);
    const QRectF visible(m_offsetX / scale, m_offsetY / scale, width() / scale, height() / scale);
    if (m_pageOutlines) {
        const auto size = NoteDocument::paperSize(m_paper);
        auto colour = m_selectionColour; colour.setAlphaF(.4); painter->setPen(QPen(colour, 1, Qt::DashLine)); painter->setBrush(Qt::NoBrush);
        for (qreal x = std::floor(visible.left() / size.width()) * size.width(); x <= visible.right(); x += size.width())
            for (qreal y = std::floor(visible.top() / size.height()) * size.height(); y <= visible.bottom(); y += size.height()) painter->drawRect(QRectF(QPointF(x, y), size));
    }
    // Match live ink's physical resolution. Fixed document-resolution tiles
    // were enlarged after pen-up, making committed strokes blurry when zoomed.
    // Qt Quick can apply display scaling through the painter rather than the
    // image's DPR. Use the complete device transform, including pixel rounding.
    const auto device = painter->deviceTransform();
    const QSizeF resolution(device.m11(), device.m22());
    // Keep tiles anchored to document coordinates. Fractional scrolling must
    // not invalidate and rerasterize the whole viewport on every frame.
    const QPointF origin;
    if (resolution != m_tileScale) {
        m_tiles.clear(); m_liveTiles.clear(); m_tileScale = resolution;
        m_tileSize = QSizeF(512 / resolution.width(), 512 / resolution.height()); m_tileOrigin = origin;
    }
    for (int y = std::floor((visible.top()-origin.y())/m_tileSize.height()); y <= std::floor((visible.bottom()-origin.y())/m_tileSize.height()); ++y)
        for (int x = std::floor((visible.left()-origin.x())/m_tileSize.width()); x <= std::floor((visible.right()-origin.x())/m_tileSize.width()); ++x) {
            const QPair<int, int> key{x, y}; auto tile = m_tiles.object(key);
            const QPointF position = origin + QPointF(x*m_tileSize.width(), y*m_tileSize.height());
            if (!tile) {
                auto rendered = new QImage(QSize(512, 512), QImage::Format_ARGB32_Premultiplied);
                rendered->fill(Qt::transparent);
                QPainter cached(rendered); cached.setRenderHint(QPainter::Antialiasing);
                cached.scale(resolution.width(), resolution.height()); cached.translate(-position);
                paintContent(cached, QRectF(position, m_tileSize)); cached.end();
                m_tiles.insert(key, rendered, std::max(1, int(rendered->sizeInBytes()/1024))); tile = m_tiles.object(key);
            }
            if (tile) painter->drawImage(QRectF(position, m_tileSize), *tile);
        }
    if (!m_searchMatches.isEmpty()) {
        const auto rectangle = [](const QVariant &v) { const auto r = v.toMap(); return QRectF(r.value("x").toDouble(), r.value("y").toDouble(), r.value("width").toDouble(), r.value("height").toDouble()); };
        auto colour = m_selectionColour; colour.setAlphaF(.32);
        painter->setPen(Qt::NoPen); painter->setBrush(colour);
        for (const auto &value : m_searchMatches) {
            const auto match = value.toMap(); if (!visible.intersects(rectangle(match.value("bounds")))) continue;
            if (match.value("handwriting").toBool() || match.value("title").toBool()) painter->drawRect(rectangle(match.value("bounds")));
            else for (const auto &v : m_elements) {
                const auto element = v.toMap(); if (element.value("id") != match.value("elementId")) continue;
                painter->save(); painter->setTransform(NoteDocument::elementTransform(element), true); painter->drawRect(rectangle(match.value("rect"))); painter->restore(); break;
            }
        }
    }
    if (!m_recognizedStroke.isEmpty()) NoteDocument::paintStroke(*painter, m_recognizedStroke);
    else if (!m_points.isEmpty()) {
        // Rasterize live and committed ink on the same grid, so pen-up cannot
        // change its sharpness. Panning reuses both grids at fractional offsets.
        const auto bounds = visible.intersected(m_liveBounds.adjusted(-m_penWidth, -m_penWidth, m_penWidth, m_penWidth));
        for (int y = std::floor(bounds.top()/m_tileSize.height()); !bounds.isEmpty() && y <= std::floor(bounds.bottom()/m_tileSize.height()); ++y)
            for (int x = std::floor(bounds.left()/m_tileSize.width()); x <= std::floor(bounds.right()/m_tileSize.width()); ++x) {
                const QPair<int, int> key{x, y}; auto tile = m_liveTiles.object(key);
                const QPointF position(x * m_tileSize.width(), y * m_tileSize.height());
                if (!tile) {
                    auto rendered = new QImage(QSize(512, 512), QImage::Format_ARGB32_Premultiplied); rendered->fill(Qt::transparent);
                    QPainter cached(rendered); cached.setRenderHint(QPainter::Antialiasing);
                    cached.scale(resolution.width(), resolution.height()); cached.translate(-position);
                    NoteDocument::paintStroke(cached, {{"type", "freehand"}, {"points", m_points}, {"width", m_penWidth}, {"colour", m_penColour.name(QColor::HexArgb)}});
                    cached.end(); m_liveTiles.insert(key, rendered, 1024); tile = m_liveTiles.object(key);
                }
                if (tile) painter->drawImage(QRectF(position, m_tileSize), *tile);
            }
    }
    if (!m_selection.isEmpty()) {
        const auto handles = selectionHandles();
        painter->setPen(QPen(m_selectionColour, 1)); painter->setBrush(Qt::NoBrush); painter->drawPolygon(selectionPolygon());
        painter->drawLine(handles[5], handles[4]);
        painter->setBrush(QColor(m_paper.value("background").toString()));
        for (int i = 0; i < handles.size(); ++i) {
            if (i == 4) painter->drawEllipse(handles[i], 5, 5);
            else { painter->save(); painter->translate(handles[i]); painter->rotate(std::atan2(m_frameTransform.m12(), m_frameTransform.m11()) * 180 / std::numbers::pi); painter->drawRect(QRectF(-4, -4, 8, 8)); painter->restore(); }
        }
    }
    if (m_selecting && m_marquee && m_dragging) { auto fill = m_selectionColour; fill.setAlphaF(.12); painter->setBrush(fill); painter->setPen(QPen(m_selectionColour, 1)); if (m_freeformSelection) painter->drawPolygon(m_lasso); else painter->drawRect(QRectF(m_press, m_pointer).normalized()); }
    if (((m_stylusToolHeld.isEmpty() ? m_tool : m_stylusToolHeld) == "eraser" || m_stylusEraser) && m_hovering && !m_touchPointer) {
        painter->setBrush(Qt::NoBrush); painter->setPen(QPen(m_selectionColour, 1));
        painter->drawEllipse(m_pointer, m_eraserSize / 2, m_eraserSize / 2);
    }

}
