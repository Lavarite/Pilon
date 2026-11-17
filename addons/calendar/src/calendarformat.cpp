#include "calendarformat.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <libical/ical.h>
#include <memory>

namespace {
using Component = std::unique_ptr<icalcomponent, decltype(&icalcomponent_free)>;
QString text(const char *value) { return QString::fromUtf8(value ? value : ""); }
QTimeZone zone(const QVariantMap &event) {
    auto name = event.value("timeZone").toString().toUtf8();
    auto value = QTimeZone(name);
    return value.isValid() ? value : QTimeZone::systemTimeZone();
}
QString timeString(icaltimetype t, const QString &tz) {
    if (t.is_date)
        return QDate(t.year, t.month, t.day).toString(Qt::ISODate);
    QTimeZone z = icaltime_is_utc(t) ? QTimeZone::utc() : QTimeZone(tz.toUtf8());
    if (!z.isValid())
        z = QTimeZone::systemTimeZone();
    return QDateTime(QDate(t.year, t.month, t.day), QTime(t.hour, t.minute, t.second), z).toString(Qt::ISODate);
}
QString propertyText(icalcomponent *c, icalproperty_kind kind) {
    auto p = icalcomponent_get_first_property(c, kind);
    return p ? text(icalproperty_get_value_as_string(p)) : QString{};
}
void remove(icalcomponent *c, icalproperty_kind kind) {
    while (auto p = icalcomponent_get_first_property(c, kind)) {
        icalcomponent_remove_property(c, p);
        icalproperty_free(p);
    }
}
icaltimetype icalTime(const QVariantMap &event, const QString &field) {
    const bool allDay = event.value("allDay").toBool();
    auto dt = CalendarFormat::dateTime(event, field).toTimeZone(zone(event));
    auto t = icaltime_null_time();
    t.year = dt.date().year();
    t.month = dt.date().month();
    t.day = dt.date().day();
    t.is_date = allDay;
    if (!allDay) {
        t.hour = dt.time().hour();
        t.minute = dt.time().minute();
        t.second = dt.time().second();
        t.zone = icaltimezone_get_builtin_timezone(zone(event).id().constData());
    }
    return t;
}
QVariantMap googleDate(const QVariantMap &e, const QString &field) {
    return e.value("allDay").toBool() ? QVariantMap{{"date", e.value(field)}}
                                      : QVariantMap{{"dateTime", e.value(field)}, {"timeZone", e.value("timeZone")}};
}
const QStringList days{"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
const QStringList msDays{"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday"};
const QStringList positions{"first", "second", "third", "fourth", "last"};
QMap<QString, QString> ruleParts(const QString &rule) {
    QMap<QString, QString> result;
    for (const auto &p : rule.split(';'))
        result[p.section('=', 0, 0)] = p.section('=', 1);
    return result;
}
QString fromWindows(QString name) {
    auto iana = QTimeZone::windowsIdToDefaultIanaId(name.toUtf8());
    return iana.isEmpty() ? name : QString::fromUtf8(iana);
}
// PATCH only edited fields so provider-owned formatting and metadata survive.
QVariantMap changedFields(QVariantMap payload, const QVariantMap &event, const QVariantMap &before,
                          const QMap<QString, QStringList> &mapping) {
    const auto nowCore = CalendarFormat::content(event), oldCore = CalendarFormat::content(before);
    for (auto it = mapping.cbegin(); it != mapping.cend(); ++it) {
        bool same = true;
        for (const auto &key : it.value()) {
            auto current = nowCore.contains(key) ? nowCore.value(key) : event.value(key, before.value(key));
            auto previous = oldCore.contains(key) ? oldCore.value(key) : before.value(key);
            same &= current == previous;
        }
        if (same)
            payload.remove(it.key());
    }
    return payload;
}
} // namespace
QDateTime CalendarFormat::dateTime(const QVariantMap &e, const QString &field) {
    const auto s = e.value(field).toString();
    if (e.value("allDay").toBool())
        return QDateTime(QDate::fromString(s.left(10), Qt::ISODate), QTime(0, 0), zone(e));
    const auto wall =
        QRegularExpression("^(\\d{4}-\\d{2}-\\d{2})[T ](\\d{2}:\\d{2}(?::\\d{2}(?:\\.\\d+)?)?)$").match(s);
    if (wall.hasMatch()) {
        auto time = QTime::fromString(wall.captured(2), Qt::ISODate);
        if (!time.isValid())
            time = QTime::fromString(wall.captured(2), "HH:mm");
        return QDateTime(QDate::fromString(wall.captured(1), Qt::ISODate), time, zone(e),
                         QDateTime::TransitionResolution::Reject);
    }
    return QDateTime::fromString(s, Qt::ISODate);
}
QString CalendarFormat::validate(const QVariantMap &e) {
    if (e.value("title").toString().trimmed().isEmpty())
        return QObject::tr("Enter an event title.");
    if (!QTimeZone(e.value("timeZone").toString().toUtf8()).isValid())
        return QObject::tr("Choose a valid time zone.");
    auto start = dateTime(e, "start"), end = dateTime(e, "end");
    if (!start.isValid() || !end.isValid() || end <= start)
        return QObject::tr("End must be after a valid start date and time.");
    const auto rule = e.value("recurrence").toString();
    if (!rule.isEmpty()) {
        auto r = icalrecurrencetype_from_string(rule.toUtf8().constData());
        const bool valid = r.freq != ICAL_NO_RECURRENCE && r.interval > 0 && r.count >= 0;
        icalrecurrencetype_clear(&r);
        if (!valid)
            return QObject::tr("The recurrence rule is invalid.");
    }
    for (const auto &v : e.value("guests").toList())
        if (!QRegularExpression("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$").match(v.toMap().value("email").toString()).hasMatch())
            return QObject::tr("Enter valid guest email addresses.");
    for (const auto &v : e.value("reminders").toList())
        if (!v.canConvert<double>() || !std::isfinite(v.toDouble()) || v.toDouble() != v.toInt() || v.toInt() < 0 ||
            v.toInt() > 525600)
            return QObject::tr("Reminder minutes must be between 0 and 525600.");
    return {};
}
QVariantMap CalendarFormat::content(const QVariantMap &e) {
    QVariantMap result;
    for (const auto &k : {"title", "start", "end", "allDay", "timeZone", "description", "location", "url", "recurrence",
                          "exdates", "rdates", "guests", "organizer", "availability", "privacy"})
        if (e.contains(k))
            result[k] = e[k];
    for (const auto &field : {"start", "end"}) {
        if (e.contains(field))
            result[field] = e.value("allDay").toBool() ? e.value(field).toString().left(10)
                                                       : dateTime(e, field).toUTC().toString(Qt::ISODate);
    }
    return result;
}
QVariantList CalendarFormat::occurrences(const QVariantMap &e, const QDateTime &from, const QDateTime &to) {
    QVariantList result;
    auto start = dateTime(e, "start"), end = dateTime(e, "end");
    if (!start.isValid() || !end.isValid())
        return result;
    const auto duration = start.secsTo(end);
    const auto dayDuration = start.date().daysTo(end.date());
    QSet<QString> emitted;
    auto add = [&](QDateTime at) {
        const auto finish = e.value("allDay").toBool() ? at.addDays(dayDuration) : at.addSecs(duration);
        const auto key =
            e.value("allDay").toBool() ? at.date().toString(Qt::ISODate) : at.toUTC().toString(Qt::ISODate);
        if (finish <= from || at >= to || emitted.contains(key))
            return;
        for (const auto &x : e.value("exdates").toList()) {
            auto ex = e;
            ex["start"] = x;
            if (dateTime(ex, "start") == at)
                return;
        }
        emitted.insert(key);
        auto item = e;
        item["occurrence"] = key;
        item["start"] = e.value("allDay").toBool() ? at.date().toString(Qt::ISODate) : at.toString(Qt::ISODate);
        item["end"] = e.value("allDay").toBool() ? finish.date().toString(Qt::ISODate) : finish.toString(Qt::ISODate);
        result.append(item);
    };
    const auto rule = e.value("recurrence").toString();
    if (rule.isEmpty())
        add(start);
    else {
        auto recur = icalrecurrencetype_from_string(rule.toUtf8());
        auto seed = icalTime(e, "start");
        auto iterator = icalrecur_iterator_new(recur, seed);
        if (iterator) {
            if (!recur.count && from > start) {
                auto lower = e;
                lower["start"] = from.addSecs(-duration).toTimeZone(zone(e)).toString(Qt::ISODate);
                if (e.value("allDay").toBool())
                    lower["start"] = from.addDays(-dayDuration).date().toString(Qt::ISODate);
                icalrecur_iterator_set_start(iterator, icalTime(lower, "start"));
            }
            for (int n = 0; n < 1000000; ++n) {
                auto t = icalrecur_iterator_next(iterator);
                if (icaltime_is_null_time(t))
                    break;
                QDateTime at(QDate(t.year, t.month, t.day), QTime(t.hour, t.minute, t.second), zone(e));
                if (at >= to)
                    break;
                add(at);
            }
            icalrecur_iterator_free(iterator);
        }
        icalrecurrencetype_clear(&recur);
    }
    for (const auto &v : e.value("rdates").toList()) {
        auto extra = e;
        extra["start"] = v;
        add(dateTime(extra, "start"));
    }
    return result;
}
QVariantList CalendarFormat::expandedEvents(const EventRecords &records, const EventRecords &calendars,
                                           const EventRecords &links, const EventRecords &instances,
                                           const QTimeZone &zone, const QDateTime &begin, const QDateTime &end) {
    QVariantList result;
    if (!begin.isValid() || !end.isValid() || end <= begin)
        return result;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        auto e = it.value();
        bool pending = false;
        for (const auto &link : links)
            if (link.value("event") == it.key() && link.value("pending") != "delete")
                pending |= !link.value("pending").toString().isEmpty();
        e["pending"] = pending;
        if (e.value("deleted").toBool() || e.value("cancelled").toBool())
            continue;
        if (!e.value("local", true).toBool() &&
            !calendars.value(e.value("source").toString()).value("enabled").toBool())
            continue;
        auto occurrences = CalendarFormat::occurrences(e, begin, end);
        if (!e.value("recurrence").toString().isEmpty() && !e.value("pending").toBool()) {
            QString source = e.value("source").toString();
            if (source.isEmpty())
                for (const auto &link : links)
                    if (link.value("event") == it.key() &&
                        calendars.value(link.value("calendar").toString()).value("provider") == "microsoft") {
                        source = link.value("calendar").toString();
                        break;
                    }
            const auto cache = instances.value(source);
            const QDateTime cacheBegin(QDate::fromString(cache.value("from").toString(), Qt::ISODate), QTime(0, 0), QTimeZone::utc());
            const QDateTime cacheEnd(QDate::fromString(cache.value("to").toString(), Qt::ISODate), QTime(0, 0), QTimeZone::utc());
            if (cacheBegin.isValid() && cacheEnd > cacheBegin) {
                const auto occurrenceKey = [&](const QString &value) {
                    auto original = e; original["start"] = value;
                    const auto at = CalendarFormat::dateTime(original, "start");
                    return e.value("allDay").toBool() ? at.date().toString(Qt::ISODate) : at.toUTC().toString(Qt::ISODate);
                };
                QSet<QString> overridden;
                for (const auto &value : cache.value("events").toList()) {
                    const auto instance = value.toMap();
                    if (instance.value("master") == it.key())
                        overridden.insert(occurrenceKey(instance.value("originalStart", instance.value("start")).toString()));
                }
                QVariantList fallback;
                for (const auto &value : occurrences) {
                    const auto occurrence = value.toMap();
                    const auto at = CalendarFormat::dateTime(occurrence, "start"), finish = CalendarFormat::dateTime(occurrence, "end");
                    if ((at < cacheEnd && finish > cacheBegin)
                        || overridden.contains(occurrenceKey(occurrence.value("occurrence", occurrence.value("start")).toString()))) continue;
                    fallback.append(occurrence);
                }
                occurrences = fallback;
                for (const auto &v : cache.value("events").toList()) {
                    auto instance = v.toMap();
                    if (instance.value("master") != it.key() || instance.value("cancelled").toBool() || instance.value("deleted").toBool())
                        continue;
                    auto o = e;
                    for (const auto &key :
                         {"title", "start", "end", "allDay", "timeZone", "description", "location", "guests", "reminders"})
                        if (instance.contains(key))
                            o[key] = instance[key];
                    o["occurrence"] = instance.value("originalStart", instance.value("start"));
                    if (CalendarFormat::dateTime(o, "end") > begin && CalendarFormat::dateTime(o, "start") < end)
                        occurrences.append(o);
                }
            }
        }
        for (const auto &v : occurrences) {
            auto o = v.toMap();
            auto start = CalendarFormat::dateTime(o, "start"), finish = CalendarFormat::dateTime(o, "end");
            if (!o.value("allDay").toBool()) {
                start = start.toTimeZone(zone);
                finish = finish.toTimeZone(zone);
            }
            o["displayDate"] = start.date().toString(Qt::ISODate);
            o["displayEndDate"] = finish.date().toString(Qt::ISODate);
            o["startMinute"] = start.time().hour() * 60 + start.time().minute();
            o["endMinute"] = finish.time().hour() * 60 + finish.time().minute();
            o["startMs"] = start.toMSecsSinceEpoch();
            o["endMs"] = finish.toMSecsSinceEpoch();
            result.append(o);
        }
    }
    std::sort(result.begin(), result.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value("startMs").toLongLong() < b.toMap().value("startMs").toLongLong();
    });
    return result;
}
QVariantMap CalendarFormat::reminderSchedule(const EventRecords &records, const EventRecords &calendars,
                                           const EventRecords &links, const EventRecords &instances,
                                           const QVariantMap &preferences, const QDateTime &now) {
    const auto from = now.addSecs(-300), through = now.addDays(8);
    QVariantList items;
    QStringList activeEvents;
    bool hasReminders = false;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const auto &event = it.value();
        if (event.value("deleted").toBool() || event.value("cancelled").toBool()
            || (!event.value("local", true).toBool() && !calendars.value(event.value("source").toString()).value("enabled").toBool())) continue;
        activeEvents.append(it.key());
        hasReminders |= !event.value("reminders").toList().isEmpty();
    }
    // Include starts a year beyond the queue horizon: reminders can be 525600 minutes early.
    const auto expanded = expandedEvents(records, calendars, links, instances,
        QTimeZone(preferences.value("timeZone").toString().toUtf8()), from, through.addDays(365));
    for (const auto &value : expanded) {
        const auto event = value.toMap();
        const auto start = dateTime(event, "start");
        for (const auto &offset : event.value("reminders").toList()) {
            const int minutes = offset.toInt();
            const auto due = start.addSecs(-qint64(minutes) * 60);
            if (!due.isValid() || due <= from || due > through) continue;
            const QJsonArray identity{event.value("id").toString(), event.value("occurrence").toString(), minutes};
            const auto key = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
            items.append(QVariantMap{{"key", key}, {"event", event.value("id")}, {"occurrence", event.value("occurrence")},
                {"minutes", minutes}, {"dueMs", due.toMSecsSinceEpoch()}, {"title", event.value("title")},
                {"text", start.toLocalTime().toString(QLocale().dateTimeFormat(QLocale::ShortFormat))}});
        }
    }
    std::sort(items.begin(), items.end(), [](const QVariant &first, const QVariant &second) {
        return first.toMap().value("dueMs").toLongLong() < second.toMap().value("dueMs").toLongLong();
    });
    return {{"fromMs", from.toMSecsSinceEpoch()}, {"throughMs", through.toMSecsSinceEpoch()},
        {"refreshAtMs", now.addDays(7).toMSecsSinceEpoch()}, {"hasReminders", hasReminders},
        {"activeEvents", activeEvents}, {"items", items}};
}

QVariantList CalendarFormat::readIcs(const QByteArray &bytes, QString *error) {
    Component root(icalparser_parse_string(bytes.constData()), icalcomponent_free);
    QVariantList result;
    if (!root || icalcomponent_isa(root.get()) != ICAL_VCALENDAR_COMPONENT) {
        if (error)
            *error = QObject::tr("Invalid iCalendar data.");
        return result;
    }
    for (auto c = icalcomponent_get_first_component(root.get(), ICAL_VEVENT_COMPONENT); c;
         c = icalcomponent_get_next_component(root.get(), ICAL_VEVENT_COMPONENT)) {
        const auto s = icalcomponent_get_dtstart(c), end = icalcomponent_get_dtend(c);
        auto p = icalcomponent_get_first_property(c, ICAL_DTSTART_PROPERTY);
        auto param = p ? icalproperty_get_first_parameter(p, ICAL_TZID_PARAMETER) : nullptr;
        QString tz = param       ? text(icalparameter_get_tzid(param))
                     : s.is_date ? QString::fromUtf8(QTimeZone::systemTimeZoneId())
                                 : "UTC";
        auto calendarTime = [&](icaltimetype value) {
            if (!QTimeZone(tz.toUtf8()).isValid() && !value.is_date) {
                if (auto custom = icalcomponent_get_timezone(root.get(), tz.toUtf8())) {
                    value.zone = custom;
                    return timeString(icaltime_convert_to_zone(value, icaltimezone_get_utc_timezone()), "UTC");
                }
            }
            return timeString(value, tz);
        };
        QVariantMap e{{"uid", propertyText(c, ICAL_UID_PROPERTY)},
                      {"title", text(icalcomponent_get_summary(c))},
                      {"start", calendarTime(s)},
                      {"end", calendarTime(end)},
                      {"timeZone", QTimeZone(tz.toUtf8()).isValid() ? tz : QString("UTC")},
                      {"allDay", bool(s.is_date)},
                      {"description", text(icalcomponent_get_description(c))},
                      {"location", text(icalcomponent_get_location(c))},
                      {"url", propertyText(c, ICAL_URL_PROPERTY)},
                      {"recurrence", propertyText(c, ICAL_RRULE_PROPERTY)},
                      {"privacy", propertyText(c, ICAL_CLASS_PROPERTY) == "PRIVATE" ? "private" : "default"},
                      {"availability", propertyText(c, ICAL_TRANSP_PROPERTY) == "TRANSPARENT" ? "free" : "busy"}};
        if (icaltime_is_null_time(end)) {
            auto dt = dateTime(e, "start");
            e["end"] = s.is_date ? dt.addDays(1).date().toString(Qt::ISODate) : dt.addSecs(3600).toString(Qt::ISODate);
        }
        if (auto rid = icalcomponent_get_first_property(c, ICAL_RECURRENCEID_PROPERTY))
            e["recurrenceId"] = timeString(icalproperty_get_recurrenceid(rid), tz);
        QVariantList exdates, rdates, guests, reminders;
        for (auto x = icalcomponent_get_first_property(c, ICAL_EXDATE_PROPERTY); x;
             x = icalcomponent_get_next_property(c, ICAL_EXDATE_PROPERTY))
            exdates.append(timeString(icalproperty_get_exdate(x), tz));
        for (auto x = icalcomponent_get_first_property(c, ICAL_RDATE_PROPERTY); x;
             x = icalcomponent_get_next_property(c, ICAL_RDATE_PROPERTY))
            rdates.append(timeString(icalproperty_get_rdate(x).time, tz));
        for (auto x = icalcomponent_get_first_property(c, ICAL_ATTENDEE_PROPERTY); x;
             x = icalcomponent_get_next_property(c, ICAL_ATTENDEE_PROPERTY)) {
            auto cn = icalproperty_get_first_parameter(x, ICAL_CN_PARAMETER),
                 status = icalproperty_get_first_parameter(x, ICAL_PARTSTAT_PARAMETER);
            guests.append(QVariantMap{
                {"email", text(icalproperty_get_attendee(x))
                              .remove(QRegularExpression("^mailto:", QRegularExpression::CaseInsensitiveOption))},
                {"name", cn ? text(icalparameter_get_cn(cn)) : ""},
                {"status",
                 status ? text(icalparameter_enum_to_string(icalparameter_get_partstat(status))) : "NEEDS-ACTION"}});
        }
        for (auto alarm = icalcomponent_get_first_component(c, ICAL_VALARM_COMPONENT); alarm;
             alarm = icalcomponent_get_next_component(c, ICAL_VALARM_COMPONENT))
            if (auto trigger = icalcomponent_get_first_property(alarm, ICAL_TRIGGER_PROPERTY)) {
                auto t = icalproperty_get_trigger(trigger);
                if (icaltime_is_null_time(t.time))
                    reminders.append(-icaldurationtype_as_int(t.duration) / 60);
            }
        e["organizer"] = propertyText(c, ICAL_ORGANIZER_PROPERTY)
                             .remove(QRegularExpression("^mailto:", QRegularExpression::CaseInsensitiveOption));
        e["exdates"] = exdates;
        e["rdates"] = rdates;
        e["guests"] = guests;
        e["reminders"] = reminders;
        e["cancelled"] = propertyText(c, ICAL_STATUS_PROPERTY) == "CANCELLED";
        result.append(e);
    }
    return result;
}
QByteArray CalendarFormat::writeIcs(const QVariantMap &e, const QByteArray &original) {
    Component root(original.isEmpty() ? icalcomponent_new_vcalendar() : icalparser_parse_string(original.constData()),
                   icalcomponent_free);
    if (!root)
        root.reset(icalcomponent_new_vcalendar());
    if (!icalcomponent_get_first_property(root.get(), ICAL_VERSION_PROPERTY))
        icalcomponent_add_property(root.get(), icalproperty_new_version("2.0"));
    if (!icalcomponent_get_first_property(root.get(), ICAL_PRODID_PROPERTY))
        icalcomponent_add_property(root.get(), icalproperty_new_prodid("-//Pilon//Calendar//EN"));
    icalcomponent *c = nullptr;
    const auto wanted = e.value("recurrenceId").toString();
    for (auto candidate = icalcomponent_get_first_component(root.get(), ICAL_VEVENT_COMPONENT); candidate;
         candidate = icalcomponent_get_next_component(root.get(), ICAL_VEVENT_COMPONENT)) {
        auto rid = icalcomponent_get_first_property(candidate, ICAL_RECURRENCEID_PROPERTY);
        if ((wanted.isEmpty() && !rid) ||
            (rid && timeString(icalproperty_get_recurrenceid(rid), e.value("timeZone").toString()) == wanted)) {
            c = candidate;
            break;
        }
    }
    if (!c) {
        c = icalcomponent_new_vevent();
        icalcomponent_add_component(root.get(), c);
    }
    if (e.value("cancelled").toBool()) {
        remove(c, ICAL_STATUS_PROPERTY);
        icalcomponent_add_property(c, icalproperty_new_status(ICAL_STATUS_CANCELLED));
    }
    remove(c, ICAL_UID_PROPERTY);
    icalcomponent_add_property(c, icalproperty_new_uid(e.value("uid", e.value("id")).toString().toUtf8()));
    icalcomponent_set_summary(c, e.value("title").toString().toUtf8());
    icalcomponent_set_description(c, e.value("description").toString().toUtf8());
    icalcomponent_set_location(c, e.value("location").toString().toUtf8());
    remove(c, ICAL_DTSTART_PROPERTY);
    remove(c, ICAL_DTEND_PROPERTY);
    remove(c, ICAL_DURATION_PROPERTY);
    auto sp = icalproperty_new_dtstart(icalTime(e, "start")), ep = icalproperty_new_dtend(icalTime(e, "end"));
    if (!e.value("allDay").toBool())
        for (auto p : {sp, ep})
            icalproperty_add_parameter(p, icalparameter_new_tzid(e.value("timeZone").toString().toUtf8()));
    icalcomponent_add_property(c, sp);
    icalcomponent_add_property(c, ep);
    if (auto z = icaltimezone_get_builtin_timezone(e.value("timeZone").toString().toUtf8()))
        if (!icalcomponent_get_first_component(root.get(), ICAL_VTIMEZONE_COMPONENT))
            if (auto zc = icaltimezone_get_component(z))
                icalcomponent_add_component(root.get(), icalcomponent_new_clone(zc));
    remove(c, ICAL_RRULE_PROPERTY);
    if (!e.value("recurrence").toString().isEmpty()) {
        auto r = icalrecurrencetype_from_string(e.value("recurrence").toString().toUtf8());
        icalcomponent_add_property(c, icalproperty_new_rrule(r));
        icalrecurrencetype_clear(&r);
    }
    remove(c, ICAL_RDATE_PROPERTY);
    for (const auto &v : e.value("rdates").toList()) {
        auto x = e;
        x["start"] = v;
        icaldatetimeperiodtype value{};
        value.time = icalTime(x, "start");
        auto p = icalproperty_new_rdate(value);
        if (!e.value("allDay").toBool())
            icalproperty_add_parameter(p, icalparameter_new_tzid(e.value("timeZone").toString().toUtf8()));
        icalcomponent_add_property(c, p);
    }
    remove(c, ICAL_EXDATE_PROPERTY);
    for (const auto &v : e.value("exdates").toList()) {
        auto x = e;
        x["start"] = v;
        auto p = icalproperty_new_exdate(icalTime(x, "start"));
        if (!e.value("allDay").toBool())
            icalproperty_add_parameter(p, icalparameter_new_tzid(e.value("timeZone").toString().toUtf8()));
        icalcomponent_add_property(c, p);
    }
    remove(c, ICAL_URL_PROPERTY);
    if (!e.value("url").toString().isEmpty())
        icalcomponent_add_property(c, icalproperty_new_url(e.value("url").toString().toUtf8()));
    remove(c, ICAL_CLASS_PROPERTY);
    icalcomponent_add_property(
        c, icalproperty_new_class(e.value("privacy") == "private" ? ICAL_CLASS_PRIVATE : ICAL_CLASS_PUBLIC));
    remove(c, ICAL_TRANSP_PROPERTY);
    icalcomponent_add_property(
        c, icalproperty_new_transp(e.value("availability") == "free" ? ICAL_TRANSP_TRANSPARENT : ICAL_TRANSP_OPAQUE));
    remove(c, ICAL_ATTENDEE_PROPERTY);
    remove(c, ICAL_ORGANIZER_PROPERTY);
    if (!e.value("organizer").toString().isEmpty())
        icalcomponent_add_property(c,
                                   icalproperty_new_organizer(("mailto:" + e.value("organizer").toString()).toUtf8()));
    for (const auto &v : e.value("guests").toList()) {
        auto g = v.toMap();
        auto p = icalproperty_new_attendee(("mailto:" + g.value("email").toString()).toUtf8());
        icalproperty_add_parameter(p, icalparameter_new_cn(g.value("name").toString().toUtf8()));
        auto status = icalparameter_string_to_enum(g.value("status", "NEEDS-ACTION").toString().toUpper().toUtf8());
        icalproperty_add_parameter(
            p, icalparameter_new_partstat(status == 0 ? ICAL_PARTSTAT_NEEDSACTION : icalparameter_partstat(status)));
        icalcomponent_add_property(c, p);
    }
    while (auto alarm = icalcomponent_get_first_component(c, ICAL_VALARM_COMPONENT)) {
        icalcomponent_remove_component(c, alarm);
        icalcomponent_free(alarm);
    }
    for (const auto &v : e.value("reminders").toList()) {
        auto alarm = icalcomponent_new_valarm();
        icalcomponent_add_property(alarm, icalproperty_new_action(ICAL_ACTION_DISPLAY));
        icalcomponent_add_property(alarm, icalproperty_new_description(e.value("title").toString().toUtf8()));
        auto t = icaltriggertype_from_int(-v.toInt() * 60);
        icalcomponent_add_property(alarm, icalproperty_new_trigger(t));
        icalcomponent_add_component(c, alarm);
    }
    icalcomponent_set_dtstamp(c, icaltime_current_time_with_zone(icaltimezone_get_utc_timezone()));
    icalcomponent_set_sequence(c, icalcomponent_get_sequence(c) + 1);
    auto str = icalcomponent_as_ical_string_r(root.get());
    QByteArray result(str);
    icalmemory_free_buffer(str);
    return result;
}
QVariantMap CalendarFormat::fromGoogle(const QVariantMap &r) {
    const auto s = r.value("start").toMap(), e = r.value("end").toMap();
    QStringList recur;
    QVariantList exdates, rdates;
    for (const auto &v : r.value("recurrence").toList()) {
        auto line = v.toString();
        if (line.startsWith("RRULE:"))
            recur.append(line.mid(6));
        if (line.startsWith("EXDATE") || line.startsWith("RDATE")) {
            const auto colon = line.indexOf(':');
            for (const auto &value : line.mid(colon + 1).split(',')) {
                auto p = icalproperty_new_from_string((line.left(colon + 1) + value).toUtf8());
                if (p) {
                    if (line.startsWith("EXDATE"))
                        exdates.append(timeString(icalproperty_get_exdate(p), s.value("timeZone", "UTC").toString()));
                    else
                        rdates.append(
                            timeString(icalproperty_get_rdate(p).time, s.value("timeZone", "UTC").toString()));
                    icalproperty_free(p);
                }
            }
        }
    }
    QVariantList guests, reminders;
    for (const auto &v : r.value("attendees").toList()) {
        auto g = v.toMap();
        guests.append(QVariantMap{{"email", g.value("email")},
                                  {"name", g.value("displayName")},
                                  {"status", g.value("responseStatus", "needsAction")}});
    }
    for (const auto &v : r.value("reminders").toMap().value("overrides").toList())
        if (v.toMap().value("method") == "popup")
            reminders.append(v.toMap().value("minutes"));
    return {
        {"uid", r.value("iCalUID")},
        {"title", r.value("summary", QObject::tr("Untitled event"))},
        {"start", s.value("dateTime", s.value("date"))},
        {"end", e.value("dateTime", e.value("date"))},
        {"allDay", s.contains("date")},
        {"timeZone", s.value("timeZone", "UTC")},
        {"description", r.value("description", "")},
        {"location", r.value("location", "")},
        {"url", r.value("source").toMap().value("url", r.value("hangoutLink", ""))},
        {"recurrence", recur.join(';')},
        {"exdates", exdates},
        {"rdates", rdates},
        {"guests", guests},
        {"reminders", reminders},
        {"organizer", r.value("organizer").toMap().value("email", "")},
        {"availability", r.value("transparency") == "transparent" ? "free" : "busy"},
        {"privacy", r.value("visibility", "default")},
        {"cancelled", r.value("status") == "cancelled"},
        {"recurrenceId",
         r.value("originalStartTime").toMap().value("dateTime", r.value("originalStartTime").toMap().value("date"))},
        {"seriesRemoteId", r.value("recurringEventId")}};
}
QVariantMap CalendarFormat::toGoogle(const QVariantMap &e, const QVariantMap &original) {
    QVariantMap r{{"summary", e.value("title")},
                  {"description", e.value("description", "")},
                  {"location", e.value("location", "")},
                  {"start", googleDate(e, "start")},
                  {"end", googleDate(e, "end")},
                  {"transparency", e.value("availability") == "free" ? "transparent" : "opaque"},
                  {"visibility", e.value("privacy", "default")}};
    QVariantList recurrence, guests, reminders;
    if (!e.value("recurrence").toString().isEmpty())
        recurrence.append("RRULE:" + e.value("recurrence").toString());
    for (const auto &v : e.value("exdates").toList()) {
        auto x = e;
        x["start"] = v;
        auto dt = dateTime(x, "start");
        recurrence.append(e.value("allDay").toBool() ? "EXDATE;VALUE=DATE:" + dt.date().toString("yyyyMMdd")
                                                     : "EXDATE:" + dt.toUTC().toString("yyyyMMdd'T'HHmmss'Z'"));
    }
    for (const auto &v : e.value("rdates").toList()) {
        auto x = e;
        x["start"] = v;
        auto dt = dateTime(x, "start");
        recurrence.append(e.value("allDay").toBool() ? "RDATE;VALUE=DATE:" + dt.date().toString("yyyyMMdd")
                                                     : "RDATE:" + dt.toUTC().toString("yyyyMMdd'T'HHmmss'Z'"));
    }
    if (!original.contains("recurringEventId"))
        r["recurrence"] = recurrence;
    if (!e.value("url").toString().isEmpty())
        r["source"] = QVariantMap{{"title", "Pilon"}, {"url", e.value("url")}};
    const QMap<QString, QString> statuses{{"NEEDS-ACTION", "needsAction"},
                                          {"ACCEPTED", "accepted"},
                                          {"DECLINED", "declined"},
                                          {"TENTATIVE", "tentative"}};
    for (const auto &v : e.value("guests").toList()) {
        auto g = v.toMap();
        guests.append(QVariantMap{{"email", g.value("email")},
                                  {"displayName", g.value("name", "")},
                                  {"responseStatus", statuses.value(g.value("status").toString(),
                                                                    g.value("status", "needsAction").toString())}});
    }
    for (auto &v : guests) {
        auto guest = v.toMap();
        for (const auto &raw : original.value("attendees").toList())
            if (raw.toMap().value("email") == guest.value("email")) {
                auto merged = raw.toMap();
                for (auto i = guest.begin(); i != guest.end(); ++i)
                    merged[i.key()] = i.value();
                v = merged;
                break;
            }
    }
    r["attendees"] = guests;
    for (const auto &v : e.value("reminders").toList())
        reminders.append(QVariantMap{{"method", "popup"}, {"minutes", v}});
    for (const auto &v : original.value("reminders").toMap().value("overrides").toList())
        if (v.toMap().value("method") != "popup")
            reminders.append(v);
    r["reminders"] = QVariantMap{{"useDefault", false}, {"overrides", reminders}};
    if (!original.isEmpty())
        return changedFields(r, e, fromGoogle(original),
                             {{"summary", {"title"}},
                              {"description", {"description"}},
                              {"location", {"location"}},
                              {"start", {"start", "allDay", "timeZone"}},
                              {"end", {"end", "allDay", "timeZone"}},
                              {"transparency", {"availability"}},
                              {"visibility", {"privacy"}},
                              {"source", {"url"}},
                              {"recurrence", {"recurrence", "exdates", "rdates"}},
                              {"attendees", {"guests"}},
                              {"reminders", {"reminders"}}});
    return r;
}
QVariantMap CalendarFormat::fromMicrosoft(const QVariantMap &r) {
    const auto s = r.value("start").toMap(), en = r.value("end").toMap();
    QString tz =
        fromWindows(r.value("recurrence")
                        .toMap()
                        .value("range")
                        .toMap()
                        .value("recurrenceTimeZone", r.value("originalStartTimeZone", s.value("timeZone", "UTC")))
                        .toString());
    if (!QTimeZone(tz.toUtf8()).isValid())
        tz = fromWindows(s.value("timeZone", "UTC").toString());
    auto stamp = [&](const QVariantMap &v) {
        auto dt = QDateTime::fromString(v.value("dateTime").toString(), Qt::ISODate);
        return QDateTime(dt.date(), dt.time(), QTimeZone(fromWindows(v.value("timeZone", "UTC").toString()).toUtf8()))
            .toTimeZone(QTimeZone(tz.toUtf8()));
    };
    const bool allDay = r.value("isAllDay").toBool();
    auto start = stamp(s), end = stamp(en);
    const auto recur = r.value("recurrence").toMap(), pattern = recur.value("pattern").toMap(),
               range = recur.value("range").toMap();
    QString rule;
    if (!pattern.isEmpty()) {
        const auto type = pattern.value("type").toString();
        rule = "FREQ=" + (type == "daily"            ? QString("DAILY")
                          : type == "weekly"         ? QString("WEEKLY")
                          : type.endsWith("Monthly") ? QString("MONTHLY")
                                                     : QString("YEARLY"));
        rule += ";INTERVAL=" + pattern.value("interval", 1).toString();
        QStringList byday;
        for (const auto &v : pattern.value("daysOfWeek").toList()) {
            int i = msDays.indexOf(v.toString());
            if (i >= 0)
                byday << days[i];
        }
        if (!byday.isEmpty())
            rule += ";BYDAY=" + byday.join(',');
        if (type.startsWith("relative"))
            rule += ";BYSETPOS=" + QString::number(pattern.value("index") == "last"
                                                       ? -1
                                                       : positions.indexOf(pattern.value("index").toString()) + 1);
        if (type.startsWith("absolute"))
            rule += ";BYMONTHDAY=" + pattern.value("dayOfMonth").toString();
        if (type.endsWith("Yearly"))
            rule += ";BYMONTH=" + pattern.value("month").toString();
        if (range.value("type") == "numbered")
            rule += ";COUNT=" + range.value("numberOfOccurrences").toString();
        else if (range.value("type") == "endDate")
            rule += ";UNTIL=" + QDate::fromString(range.value("endDate").toString(), Qt::ISODate).toString("yyyyMMdd") +
                    (allDay ? "" : "T235959Z");
    }
    QVariantList guests;
    for (const auto &v : r.value("attendees").toList()) {
        auto g = v.toMap(), a = g.value("emailAddress").toMap();
        guests.append(QVariantMap{{"email", a.value("address")},
                                  {"name", a.value("name")},
                                  {"status", g.value("status").toMap().value("response", "notResponded")}});
    }
    return {{"uid", r.value("iCalUId")},
            {"title", r.value("subject", QObject::tr("Untitled event"))},
            {"start", allDay ? start.date().toString(Qt::ISODate) : start.toString(Qt::ISODate)},
            {"end", allDay ? end.date().toString(Qt::ISODate) : end.toString(Qt::ISODate)},
            {"allDay", allDay},
            {"timeZone", tz},
            {"description", r.value("body").toMap().value("content", "")},
            {"location", r.value("location").toMap().value("displayName", "")},
            {"url", r.value("onlineMeeting").toMap().value("joinUrl", "")},
            {"recurrence", rule},
            {"guests", guests},
            {"organizer", r.value("organizer").toMap().value("emailAddress").toMap().value("address", "")},
            {"availability", r.value("showAs") == "free" ? "free" : "busy"},
            {"privacy", r.value("sensitivity") == "private" ? "private" : "default"},
            {"reminders",
             r.value("isReminderOn").toBool() ? QVariantList{r.value("reminderMinutesBeforeStart")} : QVariantList{}},
            {"cancelled", r.value("isCancelled", false)}};
}
QString CalendarFormat::microsoftUnsupported(const QVariantMap &e) {
    const auto p = ruleParts(e.value("recurrence").toString());
    if (!e.value("rdates").toList().isEmpty())
        return QObject::tr("Microsoft cannot copy additional recurrence dates in this event.");
    for (auto it = p.begin(); it != p.end(); ++it)
        if (!QStringList{"FREQ", "INTERVAL", "BYDAY", "BYMONTHDAY", "BYMONTH", "BYSETPOS", "COUNT", "UNTIL", "WKST"}
                 .contains(it.key()))
            return QObject::tr("Microsoft does not support this recurrence rule.");
    if (!p.isEmpty() && !QStringList{"DAILY", "WEEKLY", "MONTHLY", "YEARLY"}.contains(p.value("FREQ")))
        return QObject::tr("Microsoft does not support this recurrence frequency.");
    if ((p.contains("BYMONTHDAY") && (p.value("BYMONTHDAY").toInt() < 1 || p.value("BYMONTHDAY").toInt() > 31)) ||
        (p.contains("BYSETPOS") && !QStringList{"1", "2", "3", "4", "-1"}.contains(p.value("BYSETPOS"))))
        return QObject::tr("Microsoft does not support this month-day or weekday position.");
    if (p.contains("BYDAY") && p.value("FREQ") == "DAILY")
        return QObject::tr("Use a weekly pattern for selected weekdays in Microsoft calendars.");
    for (auto key : {"BYMONTHDAY", "BYMONTH", "BYSETPOS"})
        if (p.value(key).contains(','))
            return QObject::tr("Microsoft supports only one value for this recurrence field.");
    for (const auto &d : p.value("BYDAY").split(',', Qt::SkipEmptyParts))
        if (!days.contains(d))
            return QObject::tr("Use BYSETPOS for Microsoft ordinal weekday recurrence.");
    return {};
}
QVariantMap CalendarFormat::toMicrosoft(const QVariantMap &e, const QVariantMap &original) {
    auto dt = [&](const QString &f) {
        return QVariantMap{{"dateTime", dateTime(e, f).toTimeZone(zone(e)).toString("yyyy-MM-dd'T'HH:mm:ss")},
                           {"timeZone", e.value("timeZone")}};
    };
    QVariantMap r{{"subject", e.value("title")},
                  {"body", QVariantMap{{"contentType", "text"}, {"content", e.value("description", "")}}},
                  {"start", dt("start")},
                  {"end", dt("end")},
                  {"isAllDay", e.value("allDay", false)},
                  {"location", QVariantMap{{"displayName", e.value("location", "")}}},
                  {"showAs", e.value("availability", "busy")},
                  {"sensitivity", e.value("privacy") == "private" ? "private" : "normal"}};
    QVariantList attendees;
    for (const auto &v : e.value("guests").toList()) {
        auto g = v.toMap();
        attendees.append(
            QVariantMap{{"emailAddress", QVariantMap{{"address", g.value("email")}, {"name", g.value("name", "")}}},
                        {"type", "required"}});
    }
    r["attendees"] = attendees;
    auto reminders = e.value("reminders").toList();
    r["isReminderOn"] = !reminders.isEmpty();
    if (!reminders.isEmpty())
        r["reminderMinutesBeforeStart"] = reminders.first();
    const auto p = ruleParts(e.value("recurrence").toString());
    r["recurrence"] = QVariant();
    if (!p.isEmpty()) {
        const auto freq = p.value("FREQ");
        bool relative = p.contains("BYDAY") && freq != "WEEKLY";
        QString type = freq == "DAILY"    ? "daily"
                       : freq == "WEEKLY" ? "weekly"
                                          : (relative ? QString("relative") : QString("absolute")) +
                                                (freq == "YEARLY" ? "Yearly" : "Monthly");
        auto start = dateTime(e, "start").toTimeZone(zone(e)).date();
        QVariantMap pattern{{"type", type}, {"interval", p.value("INTERVAL", "1").toInt()}};
        if (freq == "WEEKLY" || relative) {
            QVariantList ds;
            for (const auto &d : p.value("BYDAY", days[start.dayOfWeek() - 1]).split(',')) {
                int i = days.indexOf(d);
                if (i >= 0)
                    ds.append(msDays[i]);
            }
            pattern["daysOfWeek"] = ds;
            pattern["firstDayOfWeek"] = msDays[std::max<qsizetype>(0, days.indexOf(p.value("WKST", "MO")))];
        }
        if (relative) {
            int i = p.value("BYSETPOS", "1").toInt();
            pattern["index"] = i == -1 ? "last" : positions[std::clamp(i - 1, 0, 3)];
        } else if (freq != "DAILY" && freq != "WEEKLY")
            pattern["dayOfMonth"] = p.value("BYMONTHDAY", QString::number(start.day())).toInt();
        if (freq == "YEARLY")
            pattern["month"] = p.value("BYMONTH", QString::number(start.month())).toInt();
        QVariantMap range{
            {"type", "noEnd"}, {"startDate", start.toString(Qt::ISODate)}, {"recurrenceTimeZone", e.value("timeZone")}};
        if (p.contains("COUNT")) {
            range["type"] = "numbered";
            range["numberOfOccurrences"] = p["COUNT"].toInt();
        } else if (p.contains("UNTIL")) {
            range["type"] = "endDate";
            range["endDate"] = QDate::fromString(p["UNTIL"].left(8), "yyyyMMdd").toString(Qt::ISODate);
        }
        r["recurrence"] = QVariantMap{{"pattern", pattern}, {"range", range}};
    }
    if (!original.isEmpty())
        return changedFields(r, e, fromMicrosoft(original),
                             {{"subject", {"title"}},
                              {"body", {"description"}},
                              {"location", {"location"}},
                              {"start", {"start", "timeZone"}},
                              {"end", {"end", "timeZone"}},
                              {"isAllDay", {"allDay"}},
                              {"showAs", {"availability"}},
                              {"sensitivity", {"privacy"}},
                              {"recurrence", {"recurrence"}},
                              {"attendees", {"guests"}},
                              {"isReminderOn", {"reminders"}},
                              {"reminderMinutesBeforeStart", {"reminders"}}});
    return r;
}
