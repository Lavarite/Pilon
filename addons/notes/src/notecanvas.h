#pragma once
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <QQuickTextDocument>
#include <QPointer>
#include <QTimer>
#include <QCache>
#include <QImage>
#include <QCursor>
#include <QElapsedTimer>
#include <QPointingDevice>
#include <QSet>
#include "notesmanager.h"

class QTabletEvent;

class NoteTextLayout : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY changed)
    Q_PROPERTY(qreal lineSpacing READ lineSpacing WRITE setLineSpacing NOTIFY changed)
    Q_PROPERTY(qreal firstBaseline READ firstBaseline NOTIFY layoutChanged)
    Q_PROPERTY(qreal firstLineHeight READ firstLineHeight NOTIFY layoutChanged)
public:
    explicit NoteTextLayout(QObject *parent = nullptr) : QObject(parent) {}
    QQuickTextDocument *document() const { return m_document; }
    qreal lineSpacing() const { return m_spacing; }
    qreal firstBaseline() const;
    qreal firstLineHeight() const;
    void setDocument(QQuickTextDocument *document);
    void setLineSpacing(qreal spacing);
signals:
    void changed();
    void layoutChanged();
private:
    void apply();
    QPointer<QQuickTextDocument> m_document;
    qreal m_spacing = 24;
    bool m_applying = false;
};

// Keep insertion formatting separate from TextSelection, whose empty selection
// applies formatting to the current word in Qt Quick.
class NoteTextFormat : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY changed)
    Q_PROPERTY(int position READ position WRITE setPosition NOTIFY changed)
    Q_PROPERTY(bool selected READ selected WRITE setSelected NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QColor pendingColour READ pendingColour NOTIFY changed)
    Q_PROPERTY(QFont pendingFont READ pendingFont NOTIFY changed)
public:
    explicit NoteTextFormat(QObject *parent = nullptr) : QObject(parent) {}
    QQuickTextDocument *document() const { return m_document; }
    void setDocument(QQuickTextDocument *document);
    int position() const { return m_position; }
    void setPosition(int position);
    bool selected() const { return m_selected; }
    void setSelected(bool selected) { m_selected = selected; if (selected) m_active = false; emit changed(); }
    bool active() const { return m_active; }
    QFont pendingFont() const { return m_font; }
    QColor pendingColour() const { return m_colour; }
    Q_INVOKABLE void prepare(const QFont &font, const QColor &colour = {}) { m_colour = colour; m_font = font; m_expected = m_position; m_active = true; emit changed(); }
signals:
    void changed();
private:
    QPointer<QQuickTextDocument> m_document;
    QFont m_font;
    QColor m_colour;
    int m_position = 0, m_expected = 0;
    bool m_active = false, m_selected = false, m_applying = false;
};

class NoteCanvas : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(NotesManager *notes READ notes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(int pointerCursor READ pointerCursor NOTIFY pointerChanged)
    Q_PROPERTY(QString tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(bool freeformSelection READ freeformSelection WRITE setFreeformSelection NOTIFY toolChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QStringList selectedIds READ selectedIds WRITE setSelectedIds NOTIFY selectionChanged)
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY documentHeightChanged)
    Q_PROPERTY(bool hasSelectedInk READ hasSelectedInk NOTIFY selectionChanged)
    Q_PROPERTY(QColor selectedInkColour READ selectedInkColour NOTIFY selectionChanged)
    Q_PROPERTY(qreal selectedInkWidth READ selectedInkWidth NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList selectionCorners READ selectionCorners NOTIFY selectionChanged)
    Q_PROPERTY(QRectF selectionBounds READ selectionBounds NOTIFY selectionChanged)
    Q_PROPERTY(QPointF pointerPosition READ pointerPosition NOTIFY pointerChanged)
    Q_PROPERTY(bool showPageOutlines READ showPageOutlines WRITE setShowPageOutlines NOTIFY viewChanged)
    Q_PROPERTY(QColor selectionColour READ selectionColour WRITE setSelectionColour NOTIFY viewChanged)
    Q_PROPERTY(QString documentId READ documentId WRITE setDocumentId NOTIFY documentChanged)
    Q_PROPERTY(QVariantList strokes READ strokes WRITE setStrokes NOTIFY strokesChanged)
    Q_PROPERTY(QVariantMap paper READ paper WRITE setPaper NOTIFY paperChanged)
    Q_PROPERTY(QVariantList elements READ elements WRITE setElements NOTIFY elementsChanged)
    Q_PROPERTY(qreal offsetX READ offsetX WRITE setOffsetX NOTIFY viewChanged)
    Q_PROPERTY(qreal offsetY READ offsetY WRITE setOffsetY NOTIFY viewChanged)
    Q_PROPERTY(bool showInk READ showInk WRITE setShowInk NOTIFY viewChanged)
    Q_PROPERTY(qreal documentWidth READ documentWidth NOTIFY documentHeightChanged)
    Q_PROPERTY(qreal documentHeight READ documentHeight NOTIFY documentHeightChanged)
    Q_PROPERTY(qreal paperScale READ paperScale NOTIFY paperScaleChanged)
    Q_PROPERTY(bool viewLocked READ viewLocked WRITE setViewLocked NOTIFY viewLockedChanged)
    Q_PROPERTY(QColor penColour READ penColour WRITE setPenColour NOTIFY toolChanged)
    Q_PROPERTY(qreal penWidth READ penWidth WRITE setPenWidth NOTIFY toolChanged)
    Q_PROPERTY(bool recognizeShapes READ recognizeShapes WRITE setRecognizeShapes NOTIFY toolChanged)
    Q_PROPERTY(bool drawEnabled READ drawEnabled WRITE setDrawEnabled NOTIFY toolChanged)
    Q_PROPERTY(bool erasing READ erasing WRITE setErasing NOTIFY toolChanged)
    Q_PROPERTY(QString eraserMode READ eraserMode WRITE setEraserMode NOTIFY toolChanged)
    Q_PROPERTY(qreal eraserSize READ eraserSize WRITE setEraserSize NOTIFY toolChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool touchAvailable READ touchAvailable NOTIFY touchAvailableChanged)
    Q_PROPERTY(bool drawWithFinger READ drawWithFinger WRITE setDrawWithFinger NOTIFY toolChanged)
    Q_PROPERTY(bool pressureSensitive READ pressureSensitive WRITE setPressureSensitive NOTIFY toolChanged)
    Q_PROPERTY(QVariantMap stylusBindings READ stylusBindings WRITE setStylusBindings NOTIFY stylusBindingsChanged)
    Q_PROPERTY(QString stylusToolHeld READ stylusToolHeld NOTIFY stylusToolHeldChanged)
    Q_PROPERTY(bool stylusDown READ stylusDown NOTIFY stylusDownChanged)
    Q_PROPERTY(qreal holdProgress READ holdProgress NOTIFY holdChanged)
    Q_PROPERTY(QPointF holdPosition READ holdPosition NOTIFY holdChanged)
public:
    explicit NoteCanvas(QQuickItem *parent = nullptr);
    NotesManager *notes() const { return m_notes; }
    void setNotes(NotesManager *notes);
    int pointerCursor() const { return int(cursor().shape()); }
    Q_INVOKABLE void trackPointer(qreal x, qreal y) { if (!stylusOnCanvas() && !m_touchPointer) updatePointer(QPointF(x, y)); }
    Q_INVOKABLE void leavePointer() { if (!stylusOnCanvas()) { m_hovering = false; update(); } }
    QString tool() const { return m_tool; }
    void setTool(const QString &tool);
    bool freeformSelection() const { return m_freeformSelection; }
    void setFreeformSelection(bool freeform) { if (m_freeformSelection == freeform) return; finishInput(); m_freeformSelection = freeform; emit toolChanged(); update(); }
    bool hasSelection() const { return !m_selection.isEmpty(); }
    QStringList selectedIds() const { return m_selection; }
    void setSelectedIds(const QStringList &ids);
    bool hasContent() const { return !m_strokes.isEmpty() || !m_elements.isEmpty(); }
    bool hasSelectedInk() const;
    QColor selectedInkColour() const;
    qreal selectedInkWidth() const;
    Q_INVOKABLE void styleSelectedInk(const QColor &colour, qreal width, bool changeColour = true, bool changeWidth = true);
    QRectF selectionBounds() const;
    QPolygonF selectionPolygon() const;
    QVariantList selectionCorners() const { QVariantList points; for (auto p : selectionPolygon()) points.append(p); return points; }
    QPointF pointerPosition() const { return m_pointer; }
    bool showPageOutlines() const { return m_pageOutlines; }
    void setShowPageOutlines(bool show) { if (show != m_pageOutlines) { m_pageOutlines = show; emit viewChanged(); update(); } }
    QColor selectionColour() const { return m_selectionColour; }
    void setSelectionColour(QColor colour) { m_selectionColour = colour; emit viewChanged(); update(); }
    Q_INVOKABLE void selectAt(qreal x, qreal y, int modifiers = 0);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void copySelection(bool cut = false);
    Q_INVOKABLE void beginEdit();
    Q_INVOKABLE void endEdit();
    Q_INVOKABLE void moveSelection(qreal dx, qreal dy);
    Q_INVOKABLE void transformSelection(qreal scale, qreal degrees);
    QString documentId() const { return m_documentId; }
    QVariantList strokes() const { return m_strokes; }
    QVariantMap paper() const { return m_paper; }
    QVariantList elements() const { return m_elements; }
    qreal offsetX() const { return m_offsetX; }
    qreal offsetY() const { return m_offsetY; }
    bool viewLocked() const { return m_viewLocked; }
    void setViewLocked(bool locked);
    bool showInk() const { return m_showInk; }
    qreal documentWidth() const;
    qreal documentHeight() const;
    qreal paperScale() const;
    Q_INVOKABLE void zoomAt(qreal factor, qreal x, qreal y);
    QColor penColour() const { return m_penColour; }
    qreal penWidth() const { return m_penWidth; }
    bool recognizeShapes() const { return m_recognize; }
    bool drawEnabled() const { return m_drawEnabled; }
    bool erasing() const { return m_eraseTool; }
    QString eraserMode() const { return m_eraserMode; }
    qreal eraserSize() const { return m_eraserSize; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool touchAvailable() const { return m_touchAvailable; }
    QVariantMap stylusBindings() const { return m_stylusBindings; }
    void setStylusBindings(const QVariantMap &bindings);
    QString stylusToolHeld() const { return m_stylusToolHeld; }
    bool stylusDown() const { return m_stylusDown; }
    bool drawWithFinger() const { return m_drawWithFinger; }
    void setDrawWithFinger(bool enabled);
    bool pressureSensitive() const { return m_pressureSensitive; }
    void setPressureSensitive(bool enabled);
    qreal holdProgress() const { return m_holdProgress; }
    QPointF holdPosition() const { return m_holdPosition; }
    Q_INVOKABLE void refreshTouchSupport();
    void setDocumentId(const QString &id);
    void setStrokes(const QVariantList &strokes);
    void setPaper(const QVariantMap &paper);
    void setElements(const QVariantList &elements);
    void setOffsetX(qreal value);
    void setOffsetY(qreal value);
    void setShowInk(bool show);
    void setPenColour(const QColor &colour);
    void setPenWidth(qreal width);
    void setRecognizeShapes(bool enabled);
    void setDrawEnabled(bool enabled);
    void setErasing(bool erasing);
    void setEraserMode(const QString &mode);
    void setEraserSize(qreal size);
    Q_INVOKABLE void beginStroke(qreal x, qreal y, qreal pressure, bool erase = false);
    Q_INVOKABLE void addPoint(qreal x, qreal y, qreal pressure);
    Q_INVOKABLE void finishStroke();
    Q_INVOKABLE void cancelStroke();
    Q_INVOKABLE void interruptInput();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    void paint(QPainter *painter) override;
protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void touchUngrabEvent() override;
    bool eventFilter(QObject *object, QEvent *event) override;
signals:
    void notesChanged();
    void selectionChanged();
    void pointerChanged();
    void elementsEdited();
    void panRequested(qreal dx, qreal dy);
    void contextMenuRequested(qreal x, qreal y);
    void textRequested(qreal x, qreal y);
    void editTextRequested(QString id);
    void documentChanged();
    void strokesChanged();
    void paperChanged();
    void elementsChanged();
    void shapeRecognized();
    void viewChanged();
    void paperScaleChanged();
    void viewLockedChanged();
    void documentHeightChanged();
    void toolChanged();
    void historyChanged();
    void touchAvailableChanged();
    void stylusBindingsChanged();
    void stylusToolHeldChanged();
    void stylusDownChanged();
    void holdChanged();
private:
    QVariantMap snapshot() const;
    void restoreSnapshot(const QVariantMap &state);
    void applyTransform(const QVariantMap &state, const QTransform &transform, qreal scaleX, qreal scaleY, qreal degrees);
    QString itemAt(const QPointF &point) const;
    QRectF itemBounds(const QString &key) const;
    void updateSelectionFrame();
    QList<QPointF> selectionHandles() const;
    void invalidateContent();
    void paintContent(QPainter &painter, const QRectF &visible);
    int handleAt(const QPointF &point) const;
    bool selectionContains(const QPointF &point) const;
    void beginSelection(const QPointF &point, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool selectedOnly = false);
    void updateSelection(const QPointF &point);
    void updatePointer(const QPointF &point);
    void handleWheel(QWheelEvent *event);
    bool ownsInputAt(const QPointF &scenePosition) const;
    QQuickItem *inputTargetAt(const QPointF &scenePosition) const;
    bool palmTouch(const QTouchEvent *event) const;
    void cancelTouch();
    void rejectTouchSequence();
    bool suppressTouchForStylus() const;
    enum class HoldSource { None, Touch, Stylus };
    void startContextHold(const QPointF &position, HoldSource source);
    void stopContextHold(HoldSource source = HoldSource::None);
    void requestContextMenu(const QPointF &position);
    void beginTouchAction(const QPointF &position);
    void finishTouchEdit(bool cancel);
    bool stylusOnCanvas() const;
    bool handleTablet(QTabletEvent *event);
    bool handleStylusMouse(QMouseEvent *event);
    void finishStylus(const char *reason = nullptr, bool releaseTool = true);
    void setStylusToolHeld(const QString &tool);
    void setStylusDown(bool down);
    void trackStylusDevice(const QPointingDevice *device);
    void logStylus(const QTabletEvent *event, const char *route);
    void requestText(const QPointF &position);
    void moveView(qreal scale, const QPointF &from, const QPointF &to);
    void finishInput();
    void publish();
    void recognizeHeldStroke();
    void adjustRecognizedStroke(const QPointF &point);
    QPointer<NotesManager> m_notes;
    QStringList m_frameSelection;
    qreal m_selectionRotation = 0, m_startRotation = 0;
    QString m_documentId;
    QVariantList m_searchMatches;
    QVariantList m_strokes;
    QVariantMap m_paper;
    QVariantList m_elements;
    QCache<QString, QImage> m_images{65536};
    QVariantList m_points;
    QVariantMap m_before;
    QList<QVariantMap> m_undo, m_redo;
    bool m_editing = false;
    QString m_tool = "pan";
    QStringList m_selection, m_marqueeBefore;
    bool m_freeformSelection = false;
    QPolygonF m_lasso;
    QPointF m_pointer{80, 140}, m_press, m_panPoint;
    QRectF m_startBounds, m_frame;
    QTransform m_startFrame, m_frameTransform;
    bool m_hovering = false;
    QCache<QPair<int, int>, QImage> m_tiles{65536};
    QCache<QPair<int, int>, QImage> m_liveTiles{16384};
    QVariantList m_cachedStrokes, m_cachedElements;
    QRectF m_contentBounds, m_liveBounds;
    QSizeF m_tileScale, m_tileSize{512, 512};
    QPointF m_tileOrigin;
    bool m_panning = false, m_selecting = false, m_dragging = false, m_marquee = false;
    int m_handle = 0;
    bool m_pageOutlines = false;
    QColor m_selectionColour{Qt::blue};
    QVariantMap m_snapOriginal;
    QColor m_penColour = QColor("#202020");
    qreal m_penWidth = 3;
    qreal m_offsetX = 0;
    qreal m_offsetY = 0;
    qreal m_zoom = 1;
    bool m_viewLocked = false;
    bool m_drawEnabled = false;
    bool m_eraseTool = false;
    QString m_eraserMode = "stroke";
    qreal m_eraserSize = 20;
    QPointF m_lastErase;
    bool m_hasErasePoint = false;
    enum class StylusAction { None, Draw, Erase, Select, Hold, Control, Consumed, Text };
    StylusAction m_stylusAction = StylusAction::None;
    QPointer<const QPointingDevice> m_stylusDevice;
    QVariantMap m_stylusBindings;
    QString m_stylusToolHeld;
    QSet<const QPointingDevice *> m_trackedStylusDevices;
    QElapsedTimer m_tabletSampleTimer;
    QPointer<const QPointingDevice> m_tabletSampleDevice;
    qreal m_stylusPressure = 0;
    bool m_stylusDown = false;
    bool m_stylusEraser = false;
    QPointF m_stylusPress;
    bool m_stylusMoved = false;
    QSet<const QPointingDevice *> m_pensInProximity;
    QElapsedTimer m_penTouchCooldown;
    QElapsedTimer m_pressureLogTimer;
    bool m_loggedContact = false, m_loggedEraser = false;
    StylusAction m_loggedAction = StylusAction::None;
    bool m_touchPointer = false, m_touchPanned = false;
    bool m_rejectedWindowTouch = false;
    enum class TouchAction { None, Pan, Select, Draw, Text, Pinch, Blocked, ContextMenu };
    TouchAction m_touchAction = TouchAction::None;
    bool m_touchEditing = false;
    bool m_touchAvailable = false, m_drawWithFinger = false;
    bool m_pressureSensitive = true;
    QTimer m_contextHoldTimer;
    QElapsedTimer m_holdElapsed;
    qreal m_holdProgress = 0;
    HoldSource m_holdSource = HoldSource::None;
    QPointF m_holdPosition;
    qreal m_touchDistance = 0;
    QPointF m_touchPress, m_touchPosition;
    int m_touchId = -1;
    QElapsedTimer m_touchTapTimer;
    QPointF m_touchTapPosition;
    QString m_touchTapItem;
    bool m_showInk = true;
    bool m_recognize = false;
    bool m_drawing = false;
    bool m_erasing = false;
    QTimer m_shapeTimer;
    QPointF m_holdAnchor;
    QVariantMap m_recognizedStroke;
};
