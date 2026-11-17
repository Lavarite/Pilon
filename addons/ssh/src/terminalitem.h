#pragma once

#include <QByteArray>
#include <QClipboard>
#include <QColor>
#include <QFont>
#include <QInputMethodEvent>
#include <QQuickPaintedItem>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <vterm.h>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

class TerminalItem : public QQuickPaintedItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(SshTerminal)

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool hasContent READ hasContent NOTIFY hasContentChanged)
    Q_PROPERTY(Qt::InputMethodHints inputMethodHints READ inputMethodHints CONSTANT)
    Q_PROPERTY(bool controlModifier READ controlModifier WRITE setControlModifier NOTIFY controlModifierChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(qreal minimumViewportHeight READ minimumViewportHeight NOTIFY fontMetricsChanged)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor
                   NOTIFY foregroundColorChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor
                   NOTIFY backgroundColorChanged)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor NOTIFY accentColorChanged)
    Q_PROPERTY(QColor warningColor READ warningColor WRITE setWarningColor NOTIFY warningColorChanged)
    Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor NOTIFY borderColorChanged)
    Q_PROPERTY(QString warningText READ warningText WRITE setWarningText NOTIFY warningTextChanged)

public:
    explicit TerminalItem(QQuickItem *parent = nullptr);
    ~TerminalItem() override;

    [[nodiscard]] bool active() const;
    void setActive(bool active);
    [[nodiscard]] bool hasContent() const;
    [[nodiscard]] Qt::InputMethodHints inputMethodHints() const;
    [[nodiscard]] bool controlModifier() const { return m_controlModifier; }
    void setControlModifier(bool enabled);
    [[nodiscard]] bool hasSelection() const { return m_hasSelection; }
    [[nodiscard]] qreal minimumViewportHeight() const;

    [[nodiscard]] QColor foregroundColor() const;
    void setForegroundColor(const QColor &color);
    [[nodiscard]] QColor backgroundColor() const;
    void setBackgroundColor(const QColor &color);
    [[nodiscard]] QColor accentColor() const;
    void setAccentColor(const QColor &color);
    [[nodiscard]] QColor warningColor() const;
    void setWarningColor(const QColor &color);
    [[nodiscard]] QColor borderColor() const;
    void setBorderColor(const QColor &color);
    [[nodiscard]] QString warningText() const;
    void setWarningText(const QString &text);

    Q_INVOKABLE void feed(const QByteArray &data);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void sendKey(int key);
    Q_INVOKABLE int scrollPosition() const { return m_scrollOffset; }
    Q_INVOKABLE void scrollFrom(int start, qreal displacement);
    Q_INVOKABLE void selectWordAt(const QPointF &point);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void copySelection(QClipboard::Mode mode = QClipboard::Clipboard) const;
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE bool clipboardHasText() const;

    void paint(QPainter *painter) override;

signals:
    void activeChanged();
    void hasContentChanged();
    void controlModifierChanged();
    void selectionChanged();
    void fontMetricsChanged();
    void foregroundColorChanged();
    void backgroundColorChanged();
    void accentColorChanged();
    void warningColorChanged();
    void borderColorChanged();
    void warningTextChanged();
    void dataGenerated(const QByteArray &data);
    void terminalSizeChanged(int rows, int columns);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    using TerminalLine = QVector<VTermScreenCell>;

    struct SelectionPosition {
        int line = 0;
        int column = 0;

        auto operator<=>(const SelectionPosition &) const = default;
    };

    static void outputCallback(const char *data, size_t length, void *user);
    static int damageCallback(VTermRect rect, void *user);
    static int moveRectCallback(VTermRect destination, VTermRect source, void *user);
    static int moveCursorCallback(VTermPos position, VTermPos oldPosition,
                                  int visible, void *user);
    static int setTermPropertyCallback(VTermProp property, VTermValue *value, void *user);
    static int bellCallback(void *user);
    static int resizeCallback(int rows, int columns, void *user);
    static int pushScrollbackCallback(int columns, const VTermScreenCell *cells, void *user);
    static int popScrollbackCallback(int columns, VTermScreenCell *cells, void *user);
    static int clearScrollbackCallback(void *user);
    static int oscFallback(int command, VTermStringFragment fragment, void *user);

    void updateTerminalSize();
    void updateDefaultColors();
    void zoomFont(int steps);
    void setHasContent(bool hasContent);
    void sendText(const QString &text, VTermModifier modifiers = VTERM_MOD_NONE);
    void pastePrimarySelection();
    void clearSelection();
    [[nodiscard]] SelectionPosition selectionPosition(const QPointF &point) const;
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool cellSelected(int visibleRow, int column) const;
    [[nodiscard]] bool documentCell(int line, int column, VTermScreenCell &cell) const;
    [[nodiscard]] QColor terminalColor(VTermColor color, bool background) const;
    [[nodiscard]] bool screenCell(int row, int column, VTermScreenCell &cell) const;
    [[nodiscard]] QPair<QString, int> inputMethodLine() const;

    VTerm *m_terminal = nullptr;
    VTermScreen *m_screen = nullptr;
    QFont m_font;
    QVector<TerminalLine> m_scrollback;
    int m_rows = 24;
    int m_columns = 80;
    int m_scrollOffset = 0;
    int m_zoomWheelDelta = 0;
    SelectionPosition m_selectionAnchor;
    SelectionPosition m_selectionEnd;
    VTermPos m_cursorPosition{0, 0};
    int m_cursorShape = VTERM_PROP_CURSORSHAPE_BLOCK;
    bool m_cursorVisible = true;
    bool m_active = false;
    bool m_controlModifier = false;
    bool m_hasContent = false;
    bool m_selecting = false;
    bool m_hasSelection = false;
    QColor m_foregroundColor{245, 245, 245};
    QColor m_backgroundColor{20, 20, 20, 220};
    QColor m_accentColor{80, 140, 240};
    QColor m_warningColor{240, 164, 60};
    QColor m_borderColor{255, 255, 255, 40};
    QString m_warningText;
    QString m_preeditText;
    QList<QInputMethodEvent::Attribute> m_preeditAttributes;
    int m_preeditCursor = 0;
    bool m_preeditCursorVisible = true;
};
