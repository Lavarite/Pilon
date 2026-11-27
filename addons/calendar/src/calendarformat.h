#pragma once
#include <QDateTime>
#include <QMap>
#include <QTimeZone>
#include <QVariantList>
#include <QVariantMap>

namespace CalendarFormat {
using EventRecords = QMap<QString, QVariantMap>;
QDateTime dateTime(const QVariantMap &event, const QString &field);
bool overlaps(const QVariantMap &event, const QDateTime &from, const QDateTime &to);
QString validate(const QVariantMap &event);
QVariantList occurrences(const QVariantMap &event, const QDateTime &from, const QDateTime &to);
QVariantList expandedEvents(const EventRecords &records, const EventRecords &calendars,
                           const EventRecords &links, const EventRecords &instances,
                           const QTimeZone &zone, const QDateTime &from, const QDateTime &to);
QVariantMap reminderSchedule(const EventRecords &records, const EventRecords &calendars,
                            const EventRecords &links, const EventRecords &instances,
                            const QVariantMap &preferences, const QDateTime &now);
QVariantList readIcs(const QByteArray &bytes, QString *error = nullptr);
QByteArray writeIcs(const QVariantMap &event, const QByteArray &original = {});
QVariantMap fromGoogle(const QVariantMap &event);
QVariantMap toGoogle(const QVariantMap &event, const QVariantMap &original = {});
QVariantMap fromMicrosoft(const QVariantMap &event);
QVariantMap toMicrosoft(const QVariantMap &event, const QVariantMap &original = {});
QString microsoftUnsupported(const QVariantMap &event);
QVariantMap content(const QVariantMap &event);
} // namespace CalendarFormat
