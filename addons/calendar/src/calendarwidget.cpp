#include "calendarmanager.h"
#include <QColor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace {
constexpr int coverageDays = 30;
constexpr int lookaheadDays = 7;
QDateTime widgetNow(const QString &iso) {
    const auto value = iso.isEmpty() ? QDateTime::currentDateTimeUtc() : QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!value.isValid() || value.timeSpec() == Qt::LocalTime) return {};
    return value.toUTC();
}
QString utc(qint64 milliseconds) {
    const auto value = QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::utc());
    return value.toUTC().toString(value.time().msec() ? Qt::ISODateWithMs : Qt::ISODate);
}
QString plain(const QString &value, int limit, bool newline = false) {
    QString result;
    int points = 0;
    for (qsizetype index = 0; index < value.size() && points < limit; ++index) {
        char32_t point = value.at(index).unicode();
        if (QChar::isHighSurrogate(point)) {
            if (index + 1 == value.size() || !value.at(index + 1).isLowSurrogate()) continue;
            point = QChar::surrogateToUcs4(value.at(index), value.at(index + 1)); ++index;
        } else if (QChar::isLowSurrogate(point)) continue;
        if (QChar::category(point) == QChar::Other_Control && !(newline && point == '\n')) point = ' ';
        result.append(QString::fromUcs4(&point, 1)); ++points;
    }
    return result.trimmed();
}
QString eventKey(const QString &id, const QString &occurrence, const QString &date) {
    const auto bytes = QJsonDocument::fromVariant(QVariantList{id, occurrence, date}).toJson(QJsonDocument::Compact);
    return "event_" + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
struct WidgetEvent {
    QVariantMap value;
    QDate startDate, endDate;
    QDateTime start, end;
    bool allDay = false;
};
}

QString CalendarManager::widgetToday(const QString &nowISO) const {
    const auto now = widgetNow(nowISO);
    const QTimeZone zone(m_preferences.value("timeZone").toString().toUtf8());
    return now.isValid() && zone.isValid() ? now.toTimeZone(zone).date().toString(Qt::ISODate) : QString{};
}

QVariantMap CalendarManager::widgetSnapshot(const QString &nowISO, double generation, const QVariantMap &palette) const {
    const auto now = widgetNow(nowISO);
    const QTimeZone zone(m_preferences.value("timeZone").toString().toUtf8());
    if (!m_active || !now.isValid() || !zone.isValid()
        || !std::isfinite(generation) || generation < 1 || generation > 9007199254740991.0 || std::floor(generation) != generation)
        return {};
    if (palette.size() != 3) return {};
    const QRegularExpression colour("\\A#(?:[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})\\z");
    for (const auto *role : {"background", "text", "accent"})
        if (palette.value(role).metaType().id() != QMetaType::QString || !colour.match(palette.value(role).toString()).hasMatch()) return {};
    const auto zoneId = QString::fromUtf8(zone.id());
    const bool clockKnown = zoneId.size() <= 128 && QTimeZone::isTimeZoneIdAvailable(zone.id())
        && QRegularExpression("\\A[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*\\z").match(zoneId).hasMatch();
    const auto today = now.toTimeZone(zone).date();
    const auto begin = today.startOfDay(zone), horizon = today.addDays(coverageDays).startOfDay(zone);
    const auto agendaEnd = today.addDays(coverageDays - 1 + lookaheadDays).startOfDay(zone);
    if (!begin.isValid() || !horizon.isValid() || !agendaEnd.isValid() || today.year() < 1 || agendaEnd.date().year() > 9999) return {};

    // Date-only events retain their own dates even when their source zone differs by more than a day.
    const auto expanded = events(today.addDays(-2).toString(Qt::ISODate), agendaEnd.date().addDays(2).toString(Qt::ISODate));
    QList<WidgetEvent> agenda;
    QSet<qint64> changes;
    changes.insert(begin.toMSecsSinceEpoch()); changes.insert(horizon.toMSecsSinceEpoch());
    for (int day = 1; day < coverageDays; ++day) {
        const auto at = today.addDays(day).startOfDay(zone);
        if (at.isValid()) changes.insert(at.toMSecsSinceEpoch());
    }
    for (const auto &item : expanded) {
        const auto value = item.toMap();
        WidgetEvent event{value, QDate::fromString(value.value("displayDate").toString(), Qt::ISODate),
            QDate::fromString(value.value("displayEndDate").toString(), Qt::ISODate),
            QDateTime::fromMSecsSinceEpoch(value.value("startMs").toLongLong(), zone),
            QDateTime::fromMSecsSinceEpoch(value.value("endMs").toLongLong(), zone), value.value("allDay").toBool()};
        if (event.allDay ? event.endDate <= today || event.startDate >= agendaEnd.date()
                         : event.end <= now || event.start >= agendaEnd) continue;
        agenda.append(event);
        const auto end = event.allDay ? event.endDate.startOfDay(zone) : event.end;
        if (end.isValid() && end > now && end < horizon) changes.insert(end.toMSecsSinceEpoch());
    }
    std::sort(agenda.begin(), agenda.end(), [](const WidgetEvent &a, const WidgetEvent &b) {
        if (a.allDay != b.allDay) return a.allDay;
        if (a.start != b.start) return a.start < b.start;
        if (a.value.value("id") != b.value.value("id")) return a.value.value("id").toString() < b.value.value("id").toString();
        return a.value.value("occurrence").toString() < b.value.value("occurrence").toString();
    });
    const auto addOngoingHour = [&](const QDateTime &at) {
        if (at.isValid() && at > now && at < horizon && std::any_of(agenda.cbegin(), agenda.cend(), [&](const WidgetEvent &event) {
                return !event.allDay && event.start < at && event.end > at;
            })) changes.insert(at.toMSecsSinceEpoch());
    };
    for (int day = 0; day < coverageDays; ++day)
        for (int hour = 0; hour < 24; ++hour)
            for (const auto resolution : {QDateTime::TransitionResolution::PreferBefore, QDateTime::TransitionResolution::PreferAfter}) {
                const auto date = today.addDays(day); const QTime time(hour, 0);
                const QDateTime at(date, time, zone, resolution);
                if (at.date() == date && at.time() == time) addOngoingHour(at);
            }
    for (const auto &transition : zone.transitions(now, horizon)) addOngoingHour(transition.atUtc);
    auto boundaries = changes.values(); std::sort(boundaries.begin(), boundaries.end());
    const auto count = std::min(qsizetype(64), boundaries.size() - 1);
    qint64 validThrough = boundaries.at(count);
    qsizetype totalRows = 0;
    QVariantList timeline;
    QVariantMap actions{{"open", QVariantMap{}}};
    const QLocale locale;
    for (qsizetype index = 0; index < count; ++index) {
        const auto at = QDateTime::fromMSecsSinceEpoch(boundaries.at(index), zone);
        const auto date = at.date();
        const auto effective = index == 0 ? now : at;
        QDate displayed;
        for (const auto &event : agenda) {
            const auto candidate = std::max(date, event.allDay ? event.startDate : event.start.date());
            if (candidate >= date.addDays(lookaheadDays) || (event.allDay ? event.endDate <= candidate : event.end <= effective)) continue;
            if (!displayed.isValid() || candidate < displayed) displayed = candidate;
        }
        if (!displayed.isValid()) displayed = date;
        const auto dayStart = displayed.startOfDay(zone), dayEnd = displayed.addDays(1).startOfDay(zone);
        if (!dayStart.isValid() || !dayEnd.isValid()) return {};
        QList<const WidgetEvent *> visible;
        QSet<QString> seen;
        for (const auto &event : agenda) {
            if (event.allDay ? event.startDate > displayed || event.endDate <= displayed
                             : event.start >= dayEnd || event.end <= (displayed == date ? effective : dayStart)) continue;
            const auto key = eventKey(event.value.value("id").toString(), event.value.value("occurrence").toString(), displayed.toString(Qt::ISODate));
            if (!seen.contains(key)) { visible.append(&event); seen.insert(key); }
        }
        const auto minuteRange = [&](const WidgetEvent &event) {
            const auto start = std::clamp(event.start.date() < displayed ? 0 : event.start.time().hour() * 60 + event.start.time().minute(), 0, 1439);
            const auto finish = event.end.date() > displayed ? 1440 : event.end.time().hour() * 60 + event.end.time().minute()
                + (event.end.time().second() || event.end.time().msec() ? 1 : 0);
            // A repeated DST hour can reverse wall times; retain actual labels and a valid minimum interval.
            return QVariantMap{{"startMinute", start}, {"endMinute", std::clamp(finish, start + 1, 1440)}};
        };
        int anchor = 540, firstMinute = 1440;
        bool ongoing = false;
        for (const auto *event : visible) if (!event->allDay) {
            firstMinute = std::min(firstMinute, minuteRange(*event).value("startMinute").toInt());
            ongoing |= displayed == date && event->start <= effective && event->end > effective;
        }
        if (ongoing) anchor = effective.toTimeZone(zone).time().hour() * 60;
        else if (firstMinute < 1440) anchor = firstMinute / 60 * 60;
        const auto shown = std::min(qsizetype(visible.size() > 64 ? 63 : 64), visible.size());
        const auto rowCount = shown + (visible.size() > shown ? 1 : 0);
        if (totalRows + rowCount > 512) { validThrough = boundaries.at(index); break; }
        totalRows += rowCount;
        const auto day = displayed.toString(Qt::ISODate);
        const auto headerId = "period_" + displayed.toString("yyyyMMdd") + '_' + QString::number(anchor);
        const QVariantMap period{{"kind", "period"}, {"date", day}, {"minute", anchor}};
        actions[headerId] = period;
        if (timeline.isEmpty()) actions["open"] = period;
        QVariantList rows;
        for (qsizetype row = 0; row < shown; ++row) {
            const auto &event = *visible.at(row);
            const auto id = event.value.value("id").toString(), occurrence = event.value.value("occurrence").toString();
            const auto key = eventKey(id, occurrence, day);
            QString leading;
            if (event.allDay) leading = tr("All day");
            else leading = locale.toString(event.start.time(), QLocale::ShortFormat) + '\n' + locale.toString(event.end.time(), QLocale::ShortFormat);
            const auto location = plain(event.value.value("location").toString(), 512, true);
            const auto title = plain(event.value.value("title").toString(), 256);
            const QColor eventColour(event.value.value("colour").toString());
            QVariantMap item{{"id", key}, {"title", title.isEmpty() ? tr("Event") : title}, {"detail", location},
                {"leading", plain(leading, 128, true)}, {"accent", eventColour.isValid()
                    ? eventColour.name(eventColour.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb) : palette.value("accent").toString()}};
            if (event.allDay) item["allDay"] = true;
            else item["timeRange"] = minuteRange(event);
            if (!id.isEmpty() && !occurrence.isEmpty() && plain(id, 1024) == id && plain(occurrence, 1024) == occurrence) {
                actions[key] = QVariantMap{{"eventId", id}, {"occurrence", occurrence}, {"date", day},
                    {"minute", event.allDay ? 540 : minuteRange(event).value("startMinute").toInt()}};
                item["actionId"] = key;
            }
            rows.append(item);
        }
        if (visible.size() > shown) {
            const auto remaining = visible.size() - shown;
            rows.append(QVariantMap{{"id", "overflow"}, {"title", remaining == 1 ? tr("1 more event in this day") : tr("%n more events in this day", nullptr, int(remaining))}, {"actionId", headerId}});
        }
        QVariantMap frame{{"at", utc(boundaries.at(index))}, {"title", plain(locale.toString(displayed, "d MMMM"), 256)},
            {"timeGrid", QVariantMap{{"startMinute", anchor}, {"endMinute", 1440}}}, {"actionId", headerId}, {"rows", rows}};
        if (clockKnown) frame["clock"] = QVariantMap{{"date", day}, {"timeZone", zoneId}};
        timeline.append(frame);
    }
    QVariantMap snapshot{{"version", 1}, {"generation", generation}, {"updatedAt", now.toString(Qt::ISODateWithMs)},
        {"validFrom", utc(begin.toMSecsSinceEpoch())}, {"validThrough", utc(validThrough)},
        {"palette", palette}, {"timeline", timeline}, {"actions", actions}};
    while (actions.size() > 512 || QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact).size() > 240 * 1024) {
        if (timeline.size() <= 1) return {};
        snapshot["validThrough"] = timeline.takeLast().toMap().value("at");
        QSet<QString> used{"open"};
        for (const auto &frame : timeline) {
            used.insert(frame.toMap().value("actionId").toString());
            for (const auto &row : frame.toMap().value("rows").toList())
                if (row.toMap().contains("actionId")) used.insert(row.toMap().value("actionId").toString());
        }
        for (auto action = actions.begin(); action != actions.end();)
            if (!used.contains(action.key())) action = actions.erase(action); else ++action;
        snapshot["timeline"] = timeline; snapshot["actions"] = actions;
    }
    return snapshot;
}
