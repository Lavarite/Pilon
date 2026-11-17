#include "notedocument.h"

#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QBuffer>
#include <functional>
#include <QFontMetricsF>
#include <QDir>
#include <QImageReader>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSharedPointer>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
QPointF point(const QVariant &value) {
    const auto p = value.toMap();
    return {p.value("x").toDouble(), p.value("y").toDouble()};
}
QVariant vertex(const QPointF &p) { return QVariantMap{{"x", p.x()}, {"y", p.y()}, {"pressure", 1.0}}; }
qreal distance(const QPointF &p, const QPointF &a, const QPointF &b) {
    const auto v = b - a;
    const qreal length = QPointF::dotProduct(v, v);
    const qreal t = length > 0 ? std::clamp(QPointF::dotProduct(p - a, v) / length, 0.0, 1.0) : 0;
    return QLineF(p, a + t * v).length();
}
QRectF geometry(const QVariantList &points) {
    if (points.isEmpty()) return {};
    qreal left = point(points[0]).x(), right = left, top = point(points[0]).y(), bottom = top;
    for (const auto &value : points) {
        const auto p = point(value);
        left = std::min(left, p.x()); right = std::max(right, p.x());
        top = std::min(top, p.y()); bottom = std::max(bottom, p.y());
    }
    return {QPointF(left, top), QPointF(right, bottom)};
}
QPolygonF simplified(const QPolygonF &points, qreal tolerance) {
    if (points.size() <= 2) return points;
    qreal largest = 0; int split = 0;
    for (int i = 1; i < points.size() - 1; ++i) {
        const auto deviation = distance(points[i], points.first(), points.last());
        if (deviation > largest) { largest = deviation; split = i; }
    }
    if (largest <= tolerance) return {points.first(), points.last()};
    auto left = simplified(points.mid(0, split + 1), tolerance);
    left.removeLast();
    return left + simplified(points.mid(split), tolerance);
}
QColor readableText(const QColor &background) {
    return background.redF() * .2126 + background.greenF() * .7152 + background.blueF() * .0722 > .5
        ? QColor(Qt::black) : QColor(Qt::white);
}
}

QVariantMap NoteDocument::defaultPaper() {
    return {{"pattern", "blank"}, {"background", "#ffffff"}, {"lines", "#d9d9d9"},
        {"spacingX", 24.}, {"spacingY", 24.}, {"lineWidth", .75}, {"size", "A4"}};
}
QVariantMap NoteDocument::normalizedPaper(const QVariantMap &paper) {
    auto result = defaultPaper();
    const auto pattern = paper.value("pattern").toString();
    if (pattern == "blank" || pattern == "lined" || pattern == "grid") result["pattern"] = pattern;
    for (const auto &key : {"background", "lines"}) {
        const QColor colour(paper.value(key).toString());
        if (colour.isValid()) result[key] = colour.name(QColor::HexRgb);
    }
    const auto size = paper.value("size").toString();
    if (size == "A2" || size == "A3" || size == "A4" || size == "A5") result["size"] = size;
    for (const auto &key : {"spacingX", "spacingY", "lineWidth"}) {
        bool ok = false; const auto value = paper.value(key).toDouble(&ok);
        if (ok && std::isfinite(value)) result[key] = key == QString("lineWidth")
            ? std::clamp(value, .25, 4.) : std::clamp(value, 12., 160.);
    }
    return result;
}
QSizeF NoteDocument::paperSize(const QVariantMap &paper) {
    const auto size = paper.value("size", "A4").toString();
    const QSizeF millimetres = size == "A2" ? QSizeF(420, 594) : size == "A3" ? QSizeF(297, 420)
        : size == "A5" ? QSizeF(148, 210) : QSizeF(210, 297);
    return millimetres * (96. / 25.4);
}
QVariantMap NoteDocument::normalizedStroke(const QVariantMap &stroke) {
    QVariantList points; const auto sourcePoints = stroke.value("points").toList(); points.reserve(sourcePoints.size());
    for (const auto &value : sourcePoints) {
        const auto map = value.toMap();
        bool xOk = false, yOk = false;
        const double x = map.value("x").toDouble(&xOk), y = map.value("y").toDouble(&yOk);
        if (!xOk || !yOk || !std::isfinite(x) || !std::isfinite(y)) return {};
        const double pressure = map.value("pressure", 1).toDouble();
        if (map.size() == 3 && map.contains("pressure") && std::isfinite(pressure) && pressure >= .05 && pressure <= 1.) points.append(value);
        else points.append(QVariantMap{{"x", x}, {"y", y}, {"pressure", std::isfinite(pressure) ? std::clamp(pressure, .05, 1.) : 1.}});
    }
    if (points.isEmpty()) return {};
    auto type = stroke.value("type", "freehand").toString();
    if (type != "freehand" && type != "line" && type != "rectangle" && type != "ellipse" && type != "triangle") return {};
    if (type != "freehand" && points.size() < (type == "triangle" ? 3 : 2)) return {};
    const QColor colour(stroke.value("colour").toString());
    const double width = stroke.value("width", 2).toDouble();
    QVariantMap result{{"type", type}, {"points", points}, {"colour", colour.isValid() ? colour.name(QColor::HexArgb) : "#ff202020"},
        {"width", std::isfinite(width) ? std::clamp(width, .5, 24.) : 2.}};
    if (stroke.contains("rotation") && std::isfinite(stroke.value("rotation").toDouble())) result["rotation"] = stroke.value("rotation").toDouble();
    const auto matrix = stroke.value("transform").toList();
    if (!matrix.isEmpty()) {
        if (matrix.size() != 6) return {};
        for (const auto &number : matrix) if (!std::isfinite(number.toDouble())) return {};
        result["transform"] = matrix;
    }
    const qreal radius = result.value("width").toDouble() / 2;
    const auto box = strokePath(result).boundingRect().adjusted(-radius, -radius, radius, radius);
    result["bounds"] = QVariantList{box.x(), box.y(), box.width(), box.height()};
    return result;
}
QRectF NoteDocument::strokeBounds(const QVariantMap &stroke) {
    const auto box = stroke.value("bounds").toList();
    if (box.size() == 4) return {box[0].toDouble(), box[1].toDouble(), box[2].toDouble(), box[3].toDouble()};
    const qreal width = stroke.value("width", 2).toDouble();
    return geometry(stroke.value("points").toList()).adjusted(-width, -width, width, width);
}
QTransform NoteDocument::strokeTransform(const QVariantMap &stroke) {
    const auto m = stroke.value("transform").toList();
    return m.size() == 6 ? QTransform(m[0].toDouble(), m[1].toDouble(), m[2].toDouble(), m[3].toDouble(), m[4].toDouble(), m[5].toDouble()) : QTransform{};
}
QPainterPath NoteDocument::strokePath(const QVariantMap &stroke) {
    const auto points = stroke.value("points").toList(); QPainterPath path;
    if (points.isEmpty()) return path;
    const auto type = stroke.value("type").toString();
    if (type == "rectangle") path.addRect(geometry(points));
    else if (type == "ellipse") path.addEllipse(geometry(points));
    else {
        path.moveTo(point(points.first()));
        if (type == "freehand" && points.size() > 2) {
            for (int i = 1; i < points.size() - 1; ++i)
                path.quadTo(point(points[i]), (point(points[i]) + point(points[i + 1])) / 2);
            path.lineTo(point(points.last()));
        } else for (int i = 1; i < points.size(); ++i) path.lineTo(point(points[i]));
        if (type == "triangle") path.closeSubpath();
    }
    return strokeTransform(stroke).map(path);
}
QVariantMap NoteDocument::transformedStroke(const QVariantMap &stroke, const QTransform &transform) {
    auto result = stroke;
    const auto m = strokeTransform(stroke) * transform;
    result["transform"] = QVariantList{m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy()};
    return normalizedStroke(result);
}
QTransform NoteDocument::elementTransform(const QVariantMap &element) {
    QTransform transform;
    transform.translate(element.value("x").toDouble() + element.value("width").toDouble() / 2,
        element.value("y").toDouble() + element.value("height").toDouble() / 2);
    transform.rotate(element.value("rotation").toDouble());
    transform.shear(element.value("shear").toDouble(), 0);
    transform.translate(-element.value("width").toDouble() / 2, -element.value("height").toDouble() / 2);
    return transform;
}
QRectF NoteDocument::elementBounds(const QVariantMap &element) {
    return elementTransform(element).mapRect(QRectF(0, 0, element.value("width").toDouble(), element.value("height").toDouble()));
}
QVariantMap NoteDocument::recognizeShape(const QVariantMap &stroke) {
    const auto values = stroke.value("points").toList();
    if (values.size() < 2) return stroke;
    QPolygonF points; qreal length = 0;
    for (const auto &value : values) {
        points.append(point(value));
        if (points.size() > 1) length += QLineF(points[points.size() - 2], points.last()).length();
    }
    const auto box = geometry(values);
    const qreal diagonal = std::hypot(box.width(), box.height());
    if (diagonal < 24 || length < 24) return stroke;
    auto result = stroke;
    const auto output = [&](const QString &type, const QVariantList &vertices) {
        auto recognized = result; recognized["type"] = type;
        QVariantList enlarged;
        const auto center = geometry(vertices).center();
        for (const auto &value : vertices) enlarged.append(vertex(center + (point(value) - center) * (type == "line" ? 1. : 1.06)));
        recognized["points"] = enlarged;
        return normalizedStroke(recognized);
    };
    qreal lineError = 0;
    for (const auto &p : points) lineError = std::max(lineError, distance(p, points.first(), points.last()));
    if (QLineF(points.first(), points.last()).length() / length > .94 && lineError < std::max(3., diagonal * .035))
        return output("line", {vertex(points.first()), vertex(points.last())});
    if (QLineF(points.first(), points.last()).length() > std::max(12., diagonal * .15)
        || box.width() < 18 || box.height() < 18) return stroke;
    // Simplify the entire closed outline, including its seam. Starting halfway
    // along a side must not introduce an extra corner in a triangle.
    int farthest = 1;
    for (int i = 2; i < points.size(); ++i)
        if (QLineF(points.first(), points[i]).length() > QLineF(points.first(), points[farthest]).length()) farthest = i;
    auto polygon = simplified(points.mid(0, farthest + 1), diagonal * .045);
    polygon.removeLast();
    auto tail = points.mid(farthest); tail.append(points.first());
    polygon += simplified(tail, diagonal * .045); polygon.removeLast();
    while (polygon.size() > 3) {
        int leastCorner = -1; qreal deviation = diagonal * .055;
        for (int i = 0; i < polygon.size(); ++i) {
            const qreal d = distance(polygon[i], polygon[(i + polygon.size() - 1) % polygon.size()], polygon[(i + 1) % polygon.size()]);
            if (d < deviation) { deviation = d; leastCorner = i; }
        }
        if (leastCorner < 0) break;
        polygon.remove(leastCorner);
    }
    if (polygon.size() == 3) {
        const auto a = polygon[1] - polygon[0], b = polygon[2] - polygon[0];
        qreal error = 0, maximum = 0, perimeter = 0;
        for (int i = 0; i < 3; ++i) perimeter += QLineF(polygon[i], polygon[(i + 1) % 3]).length();
        for (const auto &p : points) {
            const qreal d = std::min({distance(p, polygon[0], polygon[1]), distance(p, polygon[1], polygon[2]), distance(p, polygon[2], polygon[0])});
            error += d; maximum = std::max(maximum, d);
        }
        if (std::abs(a.x() * b.y() - a.y() * b.x()) > box.width() * box.height() * .4
            && error / points.size() < diagonal * .03 && maximum < diagonal * .09
            && length > perimeter * .8 && length < perimeter * 1.3)
            return output("triangle", {vertex(polygon[0]), vertex(polygon[1]), vertex(polygon[2])});
    }
    // Fit the edges before considering an ellipse. Rotated and roughly rounded
    // squares still have four corners and long straight sides.
    qreal bestError = 1e9, bestAngle = 0; QRectF bestBox;
    for (int angle = 0; angle < 90; angle += 2) {
        QTransform rotate; rotate.translate(box.center().x(), box.center().y()); rotate.rotate(-angle); rotate.translate(-box.center().x(), -box.center().y());
        const auto fitted = rotate.map(points); const auto bounds = fitted.boundingRect();
        const qreal d = std::hypot(bounds.width(), bounds.height());
        qreal edgeError = 0; bool corners[4]{};
        const QPointF at[] = {bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()};
        for (const auto &p : fitted) {
            edgeError += std::min({std::abs(p.x() - bounds.left()), std::abs(p.x() - bounds.right()),
                std::abs(p.y() - bounds.top()), std::abs(p.y() - bounds.bottom())});
            for (int i = 0; i < 4; ++i) if (std::hypot((p.x() - at[i].x()) / bounds.width(), (p.y() - at[i].y()) / bounds.height()) < .17) corners[i] = true;
        }
        const qreal error = edgeError / fitted.size() / d;
        const qreal perimeter = 2 * (bounds.width() + bounds.height());
        if (error < .05 && error < bestError && length > perimeter * .70 && length < perimeter * 1.4
            && std::all_of(std::begin(corners), std::end(corners), [](bool corner) { return corner; })) {
            bestError = error; bestAngle = angle; bestBox = bounds;
        }
    }
    if (!bestBox.isEmpty()) {
        if (std::min(bestBox.width(), bestBox.height()) / std::max(bestBox.width(), bestBox.height()) > .8) {
            const qreal side = std::max(bestBox.width(), bestBox.height());
            bestBox = QRectF(bestBox.center() - QPointF(side / 2, side / 2), QSizeF(side, side));
        }
        QTransform rotate; rotate.translate(box.center().x(), box.center().y()); rotate.rotate(bestAngle); rotate.translate(-box.center().x(), -box.center().y());
        return transformedStroke(output("rectangle", {vertex(bestBox.topLeft()), vertex(bestBox.bottomRight())}), rotate);
    }
    const qreal perimeter = 2 * (box.width() + box.height());
    qreal ellipseError = 0; bool sectors[8]{};
    for (const auto &p : points) {
        const qreal x = (p.x() - box.center().x()) / (box.width() / 2), y = (p.y() - box.center().y()) / (box.height() / 2);
        ellipseError += std::abs(std::hypot(x, y) - 1);
        const int sector = std::clamp(int((std::atan2(y, x) + std::numbers::pi) / (2 * std::numbers::pi) * 8), 0, 7);
        sectors[sector] = true;
    }
    if (ellipseError / points.size() < .14 && length < perimeter * 1.2
        && std::all_of(std::begin(sectors), std::end(sectors), [](bool sector) { return sector; })) {
        if (std::min(box.width(), box.height()) / std::max(box.width(), box.height()) >= .65) {
            const qreal radius = std::max(box.width(), box.height()) / 2;
            return output("ellipse", {vertex(box.center() - QPointF(radius, radius)), vertex(box.center() + QPointF(radius, radius))});
        }
        return output("ellipse", {vertex(box.topLeft()), vertex(box.bottomRight())});
    }
    return stroke;
}
void NoteDocument::paintPaper(QPainter &painter, const QRectF &area, const QVariantMap &paper, QPointF offset, bool titleArea) {
    painter.fillRect(area, QColor(paper.value("background", "#ffffff").toString()));
    const auto pattern = paper.value("pattern").toString();
    painter.save(); painter.setClipRect(area, Qt::IntersectClip);
    painter.setPen(QPen(QColor(paper.value("lines", "#d9d9d9").toString()), paper.value("lineWidth", .75).toDouble()));
    const qreal sx = paper.value("spacingX", 24).toDouble(), sy = paper.value("spacingY", 24).toDouble();
    const QRectF world = area.translated(offset);
    if (titleArea) {
        auto colour = pattern == "blank" ? readableText(QColor(paper.value("background").toString())) : QColor(paper.value("lines").toString());
        if (pattern == "blank") colour.setAlphaF(.45);
        painter.setPen(QPen(colour, std::max(1., paper.value("lineWidth", .75).toDouble())));
        painter.drawLine(QPointF(32 - offset.x(), 82 - offset.y()),
            QPointF(paperSize(paper).width() - 32 - offset.x(), 82 - offset.y()));
        painter.setPen(QPen(QColor(paper.value("lines", "#d9d9d9").toString()), paper.value("lineWidth", .75).toDouble()));
    }
    if (pattern != "lined" && pattern != "grid") { painter.restore(); return; }
    const qreal top = titleArea ? std::max(bodyTop, world.top()) : world.top();
    const qreal origin = titleArea ? bodyTop : 0;
    for (qreal y = origin + std::ceil((top - origin + .001) / sy) * sy; y <= world.bottom(); y += sy)
        painter.drawLine(QPointF(area.left(), y - offset.y()), QPointF(area.right(), y - offset.y()));
    if (pattern == "grid") for (qreal x = std::ceil(world.left() / sx) * sx; x <= world.right(); x += sx)
        painter.drawLine(QPointF(x - offset.x(), top - offset.y()), QPointF(x - offset.x(), area.bottom()));
    painter.restore();
}
void NoteDocument::paintStroke(QPainter &painter, const QVariantMap &stroke) {
    auto points = stroke.value("points").toList();
    if (points.isEmpty()) return;
    const qreal firstPressure = points.first().toMap().value("pressure", 1).toDouble();
    const bool uniform = std::all_of(points.cbegin(), points.cend(), [&](const QVariant &p) { return p.toMap().value("pressure", 1).toDouble() == firstPressure; });
    if (!uniform && stroke.value("type") == "freehand" && points.size() > 2) {
        QVariantList smooth{points.first()};
        QPointF start = point(points.first());
        qreal pressure = points.first().toMap().value("pressure", 1).toDouble();
        for (int i = 1; i < points.size(); ++i) {
            const auto control = point(points[i]);
            const auto end = i + 1 < points.size() ? (control + point(points[i + 1])) / 2 : control;
            const qreal cp = points[i].toMap().value("pressure", 1).toDouble();
            const qreal ep = i + 1 < points.size() ? (cp + points[i + 1].toMap().value("pressure", 1).toDouble()) / 2 : cp;
            const int steps = std::clamp(int(std::ceil((QLineF(start, control).length() + QLineF(control, end).length()) / 3)), 1, 24);
            for (int j = 1; j <= steps; ++j) {
                const qreal t = qreal(j) / steps, u = 1 - t;
                const auto position = u*u*start + 2*u*t*control + t*t*end;
                smooth.append(QVariantMap{{"x", position.x()}, {"y", position.y()}, {"pressure", u*u*pressure + 2*u*t*cp + t*t*ep}});
            }
            start = end; pressure = ep;
        }
        points = smooth;
    }
    painter.save(); const auto restore = qScopeGuard([&] { painter.restore(); });
    const auto transform = strokeTransform(stroke);
    const qreal width = stroke.value("width", 2).toDouble();
    const QColor colour(stroke.value("colour").toString());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const auto type = stroke.value("type").toString();
    if (type != "freehand") painter.drawPath(strokePath(stroke));
    else if (points.size() == 1) {
        painter.setPen(Qt::NoPen); painter.setBrush(colour);
        const qreal radius = width * points.first().toMap().value("pressure", 1).toDouble() / 2;
        painter.drawEllipse(transform.map(point(points.first())), radius, radius);
    } else {
        if (uniform) {
            painter.setPen(QPen(colour, width * firstPressure, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(strokePath(stroke)); return;
        }
        // Join sample discs with their common tangents. Width varies continuously
        // between measured pressures, including when samples are far apart.
        // Fill once so translucent ink and overlapping joins have no dark seams.
        QPainterPath outline; outline.setFillRule(Qt::WindingFill);
        QPointF previous;
        qreal previousRadius = 0;
        for (int i = 0; i < points.size(); ++i) {
            const QPointF center = transform.map(point(points[i]));
            const qreal radius = width * points[i].toMap().value("pressure", 1).toDouble() / 2;
            outline.addEllipse(center, radius, radius);
            const QPointF delta = center - previous;
            const qreal length = std::hypot(delta.x(), delta.y());
            if (i > 0 && length > std::abs(radius - previousRadius)) {
                const QPointF direction = delta / length, normal(direction.y(), -direction.x());
                const qreal slope = (previousRadius - radius) / length;
                const qreal side = std::sqrt(std::max(0., 1 - slope * slope));
                const QPointF top = direction * slope + normal * side, bottom = direction * slope - normal * side;
                outline.moveTo(previous + top * previousRadius);
                outline.lineTo(center + top * radius);
                outline.lineTo(center + bottom * radius);
                outline.lineTo(previous + bottom * previousRadius);
                outline.closeSubpath();
            }
            previous = center; previousRadius = radius;
        }
        painter.fillPath(outline, colour);
    }
}
bool NoteDocument::strokeHit(const QVariantMap &stroke, const QPointF &p, qreal radius) {
    radius += stroke.value("width", 2).toDouble() / 2;
    if (!strokeBounds(stroke).adjusted(-radius, -radius, radius, radius).contains(p)) return false;
    const auto points = stroke.value("points").toList();
    if (points.size() == 1) return QLineF(strokeTransform(stroke).map(point(points.first())), p).length() <= radius;
    for (const auto &polygon : strokePath(stroke).toSubpathPolygons())
        for (int i = 1; i < polygon.size(); ++i) if (distance(p, polygon[i - 1], polygon[i]) <= radius) return true;
    return false;
}
QVariantList NoteDocument::eraseArea(const QVariantMap &stroke, const QPointF &center, qreal radius) {
    if (stroke.contains("transform")) {
        if (!strokeHit(stroke, center, radius)) return {stroke};
        auto raw = stroke; QVariantList points;
        if (raw.value("type") == "freehand" || raw.value("type") == "line") {
            for (const auto &value : raw.value("points").toList()) {
                auto v = value.toMap(); const auto p = strokeTransform(stroke).map(point(value));
                v["x"] = p.x(); v["y"] = p.y(); points.append(v);
            }
        } else for (const auto &polygon : strokePath(stroke).toSubpathPolygons()) for (const auto &p : polygon) points.append(vertex(p));
        raw.remove("transform"); raw["type"] = "freehand"; raw["points"] = points;
        return eraseArea(normalizedStroke(raw), center, radius);
    }
    if (!strokeHit(stroke, center, radius + stroke.value("width").toDouble())) return {stroke};
    auto points = stroke.value("points").toList();
    const auto type = stroke.value("type").toString();
    const auto box = geometry(points);
    if (type == "rectangle") points = {vertex(box.topLeft()), vertex(box.topRight()), vertex(box.bottomRight()), vertex(box.bottomLeft()), vertex(box.topLeft())};
    else if (type == "triangle") points.append(points.first());
    else if (type == "ellipse") {
        points.clear();
        const int count = std::clamp(int((box.width() + box.height()) * std::numbers::pi / 2), 32, 8192);
        for (int i = 0; i <= count; ++i) {
            const qreal angle = i * 2 * std::numbers::pi / count;
            points.append(vertex(box.center() + QPointF(std::cos(angle) * box.width() / 2, std::sin(angle) * box.height() / 2)));
        }
    }
    radius += stroke.value("width", 3).toDouble() / 2;
    if (points.size() == 1) return QLineF(point(points.first()), center).length() <= radius ? QVariantList{} : QVariantList{stroke};
    QVariantList result, fragment;
    const auto flush = [&] {
        if (!fragment.isEmpty()) { auto part = stroke; part["type"] = "freehand"; part["points"] = fragment; result.append(normalizedStroke(part)); fragment.clear(); }
    };
    const auto at = [&](int index, qreal t) {
        const auto a = points[index - 1].toMap(), b = points[index].toMap();
        const QPointF p = point(a) + (point(b) - point(a)) * t;
        return QVariantMap{{"x", p.x()}, {"y", p.y()}, {"pressure", a.value("pressure", 1).toDouble() * (1 - t) + b.value("pressure", 1).toDouble() * t}};
    };
    for (int i = 1; i < points.size(); ++i) {
        const auto a = point(points[i - 1]), delta = point(points[i]) - a, relative = a - center;
        const qreal aa = QPointF::dotProduct(delta, delta), bb = 2 * QPointF::dotProduct(relative, delta);
        const qreal cc = QPointF::dotProduct(relative, relative) - radius * radius;
        QList<qreal> cuts{0, 1}; const qreal discriminant = bb * bb - 4 * aa * cc;
        if (aa > 0 && discriminant >= 0) for (qreal t : {(-bb - std::sqrt(discriminant)) / (2 * aa), (-bb + std::sqrt(discriminant)) / (2 * aa)})
            if (t > 0 && t < 1) cuts.append(t);
        std::sort(cuts.begin(), cuts.end());
        for (int j = 1; j < cuts.size(); ++j) {
            if (QLineF(a + delta * ((cuts[j - 1] + cuts[j]) / 2), center).length() <= radius) { flush(); continue; }
            if (fragment.isEmpty()) fragment.append(at(i, cuts[j - 1]));
            fragment.append(at(i, cuts[j]));
        }
    }
    flush(); return result;
}
QVector<NoteDocument::InkPage> NoteDocument::inkPages(const QVariantList &strokes, qreal pageHeight, qreal pageWidth) {
    struct Group { QRectF box; QVector<int> indices; bool importedPage = false; bool bodyText = false; };
    QVector<Group> groups;
    // Connected drawings stay together, including shapes spanning several nominal pages.
    for (int i = 0; i < strokes.size(); ++i) {
        Group group{strokeBounds(strokes[i].toMap()), {i}, strokes[i].toMap().value("type") == "page", (strokes[i].toMap().value("type") == "bodyText" || strokes[i].toMap().value("type") == "textLine")};
        for (int j = 0; j < groups.size();) {
            const qreal margin = group.bodyText || groups[j].bodyText ? 0 : 2;
            if (group.box.adjusted(-margin, -margin, margin, margin).intersects(groups[j].box)) {
                group.box = group.box.united(groups[j].box); group.indices += groups[j].indices;
                group.importedPage = group.importedPage || groups[j].importedPage;
                group.bodyText = group.bodyText || groups[j].bodyText;
                groups.removeAt(j); j = 0;
            } else ++j;
        }
        groups.append(group);
    }
    std::sort(groups.begin(), groups.end(), [](const Group &a, const Group &b) {
        return a.box.top() == b.box.top() ? a.box.left() < b.box.left() : a.box.top() < b.box.top();
    });
    // Keep a row of handwriting together without merging distant horizontal columns.
    const auto sameColumn = [pageWidth](const QRectF &a, const QRectF &b) {
        // Include the two 32px print margins when identifying a canvas column.
        return a.united(b).width() <= pageWidth + 64 || (a.left() <= b.right() && b.left() <= a.right());
    };
    QVector<Group> rows;
    for (const auto &group : groups) {
        if (!rows.isEmpty() && !group.bodyText && !rows.last().bodyText && !group.importedPage && !rows.last().importedPage && group.box.top() <= rows.last().box.bottom() + 2
            && sameColumn(rows.last().box, group.box)) {
            rows.last().box = rows.last().box.united(group.box); rows.last().indices += group.indices;
        } else rows.append(group);
    }
    QVector<InkPage> pages;
    for (const auto &group : rows) {
        bool placed = false;
        // Continue only the latest page in this column. Filling an older page
        // moves later answers ahead of the question that started a new page.
        for (auto it = pages.rbegin(); it != pages.rend(); ++it) {
            auto &page = *it;
            if (!sameColumn(page.source, group.box)) continue;
            if (group.importedPage || std::any_of(page.strokes.begin(), page.strokes.end(), [&](int index) { return strokes[index].toMap().value("type") == "page"; })) break;
            const auto combined = page.source.united(group.box);
            // Content can cross the printable margins. Fit its full width and
            // use the corresponding source height instead of detaching it.
            const qreal scale = std::min(1., pageWidth / std::max(1., combined.width()));
            if (combined.height() * scale <= pageHeight || combined.height() <= page.source.height()) {
                page.source = combined; page.strokes += group.indices; placed = true; break;
            }
            break;
        }
        if (!placed) {
            qreal left = group.box.left(), top = group.box.top();
            if (left >= 0 && group.box.right() <= pageWidth) left = 0;
            if (!group.importedPage && pages.isEmpty() && top >= 0 && group.box.bottom() <= pageHeight) top = 0;
            pages.append({QRectF(QPointF(left, top), group.box.bottomRight()), group.indices});
        }
    }
    // Spatial grouping must not change the stacking order used on the canvas.
    for (auto &page : pages) std::sort(page.strokes.begin(), page.strokes.end());
    return pages;
}
namespace NoteDocument {
static bool paintDocument(QPainter &painter, const QRectF &page, const QVariantMap &note,
                          const std::function<bool()> &nextPage, QString &error,
                          const QVariantMap *exportOptions = nullptr) {
    const auto paper = normalizedPaper(note.value("paper").toMap());
        const qreal sourceTop = exportOptions ? 32 : bodyTop;
        const QRectF content = page.adjusted(32, sourceTop, -32, -32);
        bool firstPage = true;
        int pageNumber = 0;
        QColor headingColour, pageNumberColour;
        const auto paintHeading = [&](qreal baseline = 80) {
            painter.save();
            painter.setPen(headingColour);
            QFont titleFont; titleFont.setPixelSize(24); painter.setFont(titleFont);
            painter.drawText(QPointF(32, baseline), note.value("title").toString());
            painter.restore();
        };
        const auto paintOverlays = [&] {
            if (!exportOptions) return;
            if (pageNumber == 1 || !exportOptions->value("firstPageHeadingOnly", true).toBool()) paintHeading(pageNumber == 1 ? 80 : 24);
            if (exportOptions->value("pageNumbers", true).toBool()) {
                painter.save();
                painter.setPen(pageNumberColour);
                QFont font; font.setPixelSize(12); painter.setFont(font);
                painter.drawText(page.adjusted(16, 16, -16, -16), Qt::AlignRight | Qt::AlignBottom, QString::number(pageNumber));
                painter.restore();
            }
        };
        const auto beginPage = [&](bool pattern = true) {
            if (!firstPage && !nextPage()) return false;
            firstPage = false;
            ++pageNumber;
            headingColour = pageNumberColour = readableText(QColor(paper.value("background").toString()));
            if (pattern) paintPaper(painter, page, paper);
            else {
                painter.fillRect(page, QColor(paper.value("background").toString()));
                if (!exportOptions || pageNumber == 1) paintPaper(painter, QRectF(0, 0, page.width(), bodyTop), paper);
            }
            if (!exportOptions) paintHeading();
            return true;
        };
        QVariantList entries;
        QTextDocument body;
        QFont bodyFont; bodyFont.setPixelSize(16); body.setDefaultFont(bodyFont);
        body.setDocumentMargin(0);
        auto textOption = body.defaultTextOption(); textOption.setWrapMode(QTextOption::NoWrap); body.setDefaultTextOption(textOption);
        if (note.value("html").toString().isEmpty()) body.setMarkdown(note.value("text").toString());
        else body.setHtml(note.value("html").toString());
        qreal textOffset = 0;
        if (!body.toPlainText().trimmed().isEmpty()) {
            for (auto block = body.begin(); block.isValid(); block = block.next()) {
                qreal height = QFontMetricsF(body.defaultFont()).height();
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (fragment.isValid()) height = std::max(height, QFontMetricsF(fragment.charFormat().font().resolve(body.defaultFont())).height());
                }
                const qreal spacing = paper.value("spacingY").toDouble();
                QTextCursor cursor(block); auto format = block.blockFormat();
                format.setLineHeight(std::ceil(height / spacing) * spacing, QTextBlockFormat::FixedHeight);
                format.setTopMargin(0); format.setBottomMargin(0); cursor.setBlockFormat(format);
            }
            body.setTextWidth(content.width());
            const auto first = body.begin();
            const auto firstBounds = body.documentLayout()->blockBoundingRect(first);
            if (first.layout()->lineCount() > 0) {
                const auto line = first.layout()->lineAt(0);
                textOffset = first.blockFormat().lineHeight() - firstBounds.top() - line.y() - line.ascent();
            }
            textOffset += bodyTop - sourceTop;
            // Plan at text-line boundaries, allowing images and ink on the same page
            // while keeping long notes from shrinking into one oversized drawing.
            for (auto block = body.begin(); block.isValid(); block = block.next()) {
                const auto bounds = body.documentLayout()->blockBoundingRect(block);
                for (int i = 0; i < block.layout()->lineCount(); ++i) {
                    const auto line = block.layout()->lineAt(i);
                    const QRectF box(bounds.left() + line.x(), bounds.top() + line.y() + textOffset,
                        std::max(1., line.naturalTextWidth()), line.ascent() + line.descent());
                    entries.append(QVariantMap{{"type", "bodyText"}, {"bounds", QVariantList{box.x(), box.y(), box.width(), box.height()}}});
                }
            }
        }
        // Bounds participate in the same page planner as ink so annotations stay
        // with their image or imported page. Text fields paint above the ink.
        QHash<int, QSharedPointer<QTextDocument>> textDocuments;
        for (const auto &value : note.value("elements").toList()) {
            auto item = value.toMap();
            item["x"] = item.value("x").toDouble() - 32;
            item["y"] = item.value("y").toDouble() - sourceTop;
            if (item.value("type") == "text") {
                auto document = QSharedPointer<QTextDocument>::create(); QFont font; font.setPixelSize(16);
                document->setDefaultFont(font); document->setDocumentMargin(0); document->setHtml(item.value("html").toString());
                document->setTextWidth(item.value("width").toDouble());
                const int key = textDocuments.size(); textDocuments.insert(key, document); item["layoutKey"] = key;
                item["height"] = std::max(item.value("height").toDouble(), document->size().height());
                if (document->size().height() > content.height() && std::abs(std::remainder(item.value("rotation").toDouble(), 360.)) < .001) {
                    for (auto block = document->begin(); block.isValid(); block = block.next()) {
                        const auto bounds = document->documentLayout()->blockBoundingRect(block);
                        for (int i = 0; i < block.layout()->lineCount(); ++i) {
                            const auto line = block.layout()->lineAt(i);
                            const QRectF clip(bounds.left() + line.x(), bounds.top() + line.y(), std::max(1., line.naturalTextWidth()), line.ascent() + line.descent());
                            const auto world = clip.translated(item.value("x").toDouble(), item.value("y").toDouble());
                            auto fragment = item; fragment["type"] = "textLine"; fragment["clip"] = clip;
                            fragment["bounds"] = QVariantList{world.x(), world.y(), world.width(), world.height()}; entries.append(fragment);
                        }
                    }
                    continue;
                }
            }
            const auto bounds = elementBounds(item);
            item["bounds"] = QVariantList{bounds.x(), bounds.y(), bounds.width(), bounds.height()};
            entries.append(item);
        }
        for (const auto &value : note.value("strokes").toList())
            entries.append(transformedStroke(value.toMap(), QTransform::fromTranslate(-32, -sourceTop)));
        for (const auto &inkPage : inkPages(entries, content.height(), content.width())) {
            if (!beginPage(false)) { error = QObject::tr("Unable to render a note page."); return false; }
            const bool importedPage = exportOptions && std::any_of(inkPage.strokes.cbegin(), inkPage.strokes.cend(), [&](int index) {
                return entries[index].toMap().value("type") == "page";
            });
            // Imported sheets keep their full page; export labels never reserve body space.
            const QRectF target = importedPage ? page : content;
            const qreal fittedScale = std::min(target.width() / std::max(1., inkPage.source.width()), target.height() / std::max(1., inkPage.source.height()));
            const qreal scale = importedPage ? fittedScale : std::min(1., fittedScale);
            painter.save(); painter.setRenderHint(QPainter::Antialiasing); painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.translate(target.left(), target.top());
            painter.scale(scale, scale); painter.translate(-inkPage.source.left(), -inkPage.source.top());
            painter.save();
            if (exportOptions && pageNumber == 1 && !importedPage)
                painter.setClipRect(QRectF(inkPage.source.left(), bodyTop - sourceTop, target.width() / scale, target.height() / scale), Qt::IntersectClip);
            paintPaper(painter, QRectF(inkPage.source.topLeft(), QSizeF(target.width() / scale, target.height() / scale)), paper, QPointF(32, sourceTop - bodyTop), false);
            painter.restore();
            for (const auto index : inkPage.strokes) {
                const auto item = entries[index].toMap();
                if (item.value("type") != "image" && item.value("type") != "page") continue;
                QImageReader reader(QDir(note.value("assetRoot").toString()).filePath(item.value("asset").toString()));
                reader.setAutoTransform(true);
                const auto image = reader.read();
                if (image.isNull()) { error = QObject::tr("Unable to read an imported image: %1").arg(reader.errorString()); return false; }
                painter.save(); painter.setTransform(elementTransform(item), true);
                const QRectF imageArea(0, 0, item.value("width").toDouble(), item.value("height").toDouble());
                painter.drawImage(imageArea, image);
                if (exportOptions) {
                    bool invertible = false;
                    const auto inverse = painter.transform().inverted(&invertible);
                    const auto overlayColour = [&](const QPointF &position, const QColor &fallback) {
                        const auto local = inverse.map(position);
                        if (!invertible || !imageArea.contains(local)) return fallback;
                        const auto pixel = image.pixelColor(std::clamp(int(local.x() / imageArea.width() * image.width()), 0, image.width() - 1),
                            std::clamp(int(local.y() / imageArea.height() * image.height()), 0, image.height() - 1));
                        const QColor background(paper.value("background").toString());
                        const qreal alpha = pixel.alphaF();
                        return readableText(QColor::fromRgbF(pixel.redF() * alpha + background.redF() * (1 - alpha),
                            pixel.greenF() * alpha + background.greenF() * (1 - alpha), pixel.blueF() * alpha + background.blueF() * (1 - alpha)));
                    };
                    headingColour = overlayColour(QPointF(48, pageNumber == 1 ? 68 : 12), headingColour);
                    pageNumberColour = overlayColour(page.bottomRight() - QPointF(24, 24), pageNumberColour);
                }
                painter.restore();
            }
            for (const auto index : inkPage.strokes) {
                const auto item = entries[index].toMap();
                if (item.value("type") != "bodyText") continue;
                const auto clip = strokeBounds(item).adjusted(-1, -1, 1, 1);
                QAbstractTextDocumentLayout::PaintContext context;
                context.palette.setColor(QPalette::Text, readableText(QColor(paper.value("background").toString())));
                context.clip = clip.translated(0, -textOffset);
                painter.save(); painter.setClipRect(clip); painter.translate(0, textOffset);
                body.documentLayout()->draw(&painter, context); painter.restore();
            }
            for (const auto index : inkPage.strokes) {
                const auto item = entries[index].toMap();
                if (item.contains("points")) paintStroke(painter, item);
            }
            for (const auto index : inkPage.strokes) {
                const auto item = entries[index].toMap();
                if (item.value("type") != "text" && item.value("type") != "textLine") continue;
                const auto document = textDocuments.value(item.value("layoutKey").toInt());
                QAbstractTextDocumentLayout::PaintContext context;
                context.palette.setColor(QPalette::Text, readableText(QColor(paper.value("background").toString())));
                painter.save();
                if (item.value("type") == "textLine") {
                    context.clip = item.value("clip").toRectF().adjusted(-1, -1, 1, 1);
                    painter.setClipRect(strokeBounds(item).adjusted(-1, -1, 1, 1));
                }
                painter.setTransform(elementTransform(item), true);
                document->documentLayout()->draw(&painter, context); painter.restore();
            }
            painter.restore();
            paintOverlays();
        }
        if (firstPage) {
            if (!beginPage()) { error = QObject::tr("Unable to render the note."); return false; }
            paintOverlays();
        }
    return true;
}
}
bool NoteDocument::exportPdf(const QString &path, const QVariantMap &note, QString &error, const QVariantMap &options) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
    {
        QPdfWriter writer(&file);
        writer.setResolution(96);
        const auto paper = normalizedPaper(note.value("paper").toMap());
        const auto size = paper.value("size").toString();
        writer.setPageSize(QPageSize(size == "A2" ? QPageSize::A2 : size == "A3" ? QPageSize::A3 : size == "A5" ? QPageSize::A5 : QPageSize::A4));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setTitle(note.value("title").toString());
        writer.setCreator("Pilon");
        QPainter painter;
        if (!painter.begin(&writer)) { error = QObject::tr("Unable to create the PDF."); return false; }
        const QRectF page(0, 0, writer.width(), writer.height());
        if (!paintDocument(painter, page, note, [&] { return writer.newPage(); }, error, &options)) return false;
        painter.end();
    }
    if (!file.commit()) { error = file.errorString(); return false; }
    return true;
}

QVariantList NoteDocument::renderPages(const QVariantMap &note, QString &error) {
    QVariantList pages;
    const QSize size = paperSize(normalizedPaper(note.value("paper").toMap())).toSize();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    const auto finish = [&] {
        painter.end();
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) { error = QObject::tr("Unable to encode the note preview."); return false; }
        pages.append(QVariantMap{{"mime", "image/png"}, {"width", size.width()}, {"height", size.height()},
                                 {"data", QString::fromLatin1(png.toBase64())}});
        return true;
    };
    if (!paintDocument(painter, QRectF(QPointF(), size), note, [&] {
        if (!finish()) return false;
        image.fill(Qt::white); return painter.begin(&image);
    }, error)) return {};
    if (!finish()) return {};
    return pages;
}
