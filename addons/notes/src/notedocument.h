#pragma once

#include <QColor>
#include <QPainter>
#include <QRectF>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QTransform>
#include <QPainterPath>

namespace NoteDocument {
constexpr qreal paperWidth = 794;
constexpr qreal paperHeight = 1123;
constexpr qreal bodyTop = 96;
struct InkPage {
    QRectF source;
    QVector<int> strokes;
};
QVariantMap defaultPaper();
QVariantMap normalizedPaper(const QVariantMap &paper);
QSizeF paperSize(const QVariantMap &paper);
QVariantMap normalizedStroke(const QVariantMap &stroke);
QRectF strokeBounds(const QVariantMap &stroke);
QPainterPath strokePath(const QVariantMap &stroke);
QTransform strokeTransform(const QVariantMap &stroke);
QVariantMap transformedStroke(const QVariantMap &stroke, const QTransform &transform);
QRectF elementBounds(const QVariantMap &element);
QTransform elementTransform(const QVariantMap &element);
QVariantMap recognizeShape(const QVariantMap &stroke);
void paintPaper(QPainter &painter, const QRectF &area, const QVariantMap &paper, QPointF offset = {}, bool titleArea = true);
void paintStroke(QPainter &painter, const QVariantMap &stroke);
bool strokeHit(const QVariantMap &stroke, const QPointF &point, qreal radius);
QVariantList eraseArea(const QVariantMap &stroke, const QPointF &point, qreal radius);
QVector<InkPage> inkPages(const QVariantList &strokes, qreal pageHeight = paperHeight - 80, qreal pageWidth = paperWidth);
QVariantList renderPages(const QVariantMap &note, QString &error);
bool exportPdf(const QString &path, const QVariantMap &note, QString &error, const QVariantMap &options = {});
}
