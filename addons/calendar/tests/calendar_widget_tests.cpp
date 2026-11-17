#include "calendarmanager.h"
#include "widgetstore.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLocale>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QUrlQuery>
#include <limits>

class CalendarWidgetTests : public QObject {
    Q_OBJECT
    const QVariantMap palette{{"background", "#241e1a"}, {"text", "#d3c4b5"}, {"accent", "#ffb86c"}};
    QVariantList frames(const QVariantMap &snapshot) const { return snapshot.value("timeline").toList(); }
    QVariantList rows(const QVariantMap &snapshot, int frame = 0) const { return frames(snapshot).value(frame).toMap().value("rows").toList(); }
    QVariantMap at(const QVariantMap &snapshot, const QString &iso) const {
        QVariantMap result;
        const auto time = QDateTime::fromString(iso, Qt::ISODate);
        for (const auto &value : frames(snapshot))
            if (QDateTime::fromString(value.toMap().value("at").toString(), Qt::ISODate) <= time) result = value.toMap();
        return result;
    }
    QVariantList onDay(const QVariantMap &snapshot, const QString &iso, const QString &day) const {
        QVariantList result;
        const auto actions = snapshot.value("actions").toMap();
        for (const auto &row : at(snapshot, iso).value("rows").toList())
            if (actions.value(row.toMap().value("actionId").toString()).toMap().value("date") == day) result.append(row);
        return result;
    }
    QVariantMap header(const QVariantMap &snapshot, const QString &iso) const {
        return snapshot.value("actions").toMap().value(at(snapshot, iso).value("actionId", "open").toString()).toMap();
    }
    QString add(CalendarManager &manager, const QString &title, const QString &start, const QString &end,
                bool allDay = false, const QString &zone = "UTC", const QString &rule = {}) {
        auto event = manager.newEvent(start.left(10));
        event["title"] = title; event["start"] = start; event["end"] = end;
        event["allDay"] = allDay; event["timeZone"] = zone; event["recurrence"] = rule;
        return manager.saveEvent(event);
    }
    void checkBounds(const QVariantMap &snapshot) {
        QVERIFY(!snapshot.isEmpty());
        const auto list = frames(snapshot); QVERIFY(!list.isEmpty()); QVERIFY(list.size() <= 64);
        QCOMPARE(list.first().toMap().value("at"), snapshot.value("validFrom"));
        qint64 previous = std::numeric_limits<qint64>::min(); int total = 0;
        for (const auto &frame : list) {
            const auto time = QDateTime::fromString(frame.toMap().value("at").toString(), Qt::ISODate).toMSecsSinceEpoch();
            QVERIFY(time > previous); previous = time;
            const auto entries = frame.toMap().value("rows").toList(); QVERIFY(entries.size() <= 64); total += entries.size();
        }
        QVERIFY(total <= 512);
        QVERIFY(QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact).size() <= 240 * 1024);
        const auto from = QDateTime::fromString(snapshot.value("validFrom").toString(), Qt::ISODate);
        const auto through = QDateTime::fromString(snapshot.value("validThrough").toString(), Qt::ISODate);
        const auto updated = QDateTime::fromString(snapshot.value("updatedAt").toString(), Qt::ISODate);
        QVERIFY(from <= updated); QVERIFY(updated < through);
        QVERIFY(previous < through.toMSecsSinceEpoch());
    }
private slots:
    void clockUsesDisplayedDateAndConfiguredZoneInsteadOfDeviceOrSourceZone() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Pacific/Kiritimati"}});
        QVERIFY(!add(manager, "Future day", "2026-11-10T09:00:00+14:00", "2026-11-10T10:00:00+14:00", false, "Pacific/Kiritimati").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-08T22:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(manager.widgetToday("2026-11-08T22:00:00Z"), "2026-11-09");
        QCOMPARE(frames(snapshot).first().toMap().value("clock").toMap(), (QVariantMap{{"date", "2026-11-10"}, {"timeZone", "Pacific/Kiritimati"}}));
        for (const auto &value : frames(snapshot)) {
            const auto frame = value.toMap(); const auto period = snapshot.value("actions").toMap().value(frame.value("actionId").toString()).toMap();
            QCOMPARE(frame.value("clock").toMap().value("date"), period.value("date"));
            QCOMPARE(frame.value("clock").toMap().value("timeZone").toString(), "Pacific/Kiritimati");
        }
        manager.setPreferences({{"timeZone", "Europe/London"}});
        const auto changed = manager.widgetSnapshot("2026-11-08T22:00:00Z", 2, palette); checkBounds(changed);
        QCOMPARE(frames(changed).first().toMap().value("clock").toMap(), (QVariantMap{{"date", "2026-11-09"}, {"timeZone", "Europe/London"}}));
    }
    void validCustomOffsetKeepsWidgetWithoutUnsupportedClockContext() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC+05:30"}});
        QCOMPARE(manager.preferences().value("timeZone").toString(), "UTC+05:30");
        const auto snapshot = manager.widgetSnapshot("2026-11-09T00:00:00Z", 1, palette); checkBounds(snapshot);
        for (const auto &frame : frames(snapshot)) QVERIFY(!frame.toMap().contains("clock"));
    }
    void oneDayGridSelectsNextRelevantDateWithLocaleColoursAndRanges() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Pacific/Kiritimati"}});
        const auto id = add(manager, "Tomorrow", "2026-11-10T09:15:00+14:00", "2026-11-10T10:30:00+14:00", false, "Pacific/Kiritimati");
        QVERIFY(!id.isEmpty()); auto event = manager.event(id); event["location"] = "Room 2"; event["colour"] = "tomato"; QVERIFY(!manager.saveEvent(event).isEmpty());
        QVERIFY(!add(manager, "Another day", "2026-11-11", "2026-11-12", true).isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-08T22:00:00Z", 1, palette); checkBounds(snapshot);
        const auto frame = frames(snapshot).first().toMap();
        QVERIFY(!frame.contains("heading")); QCOMPARE(frame.value("title").toString(), "10 November");
        QCOMPARE(frame.value("timeGrid").toMap(), (QVariantMap{{"startMinute", 540}, {"endMinute", 1440}}));
        QCOMPARE(rows(snapshot).size(), 1); const auto row = rows(snapshot).first().toMap();
        QCOMPARE(row.value("title").toString(), "Tomorrow"); QVERIFY(!row.contains("group"));
        QCOMPARE(row.value("timeRange").toMap(), (QVariantMap{{"startMinute", 555}, {"endMinute", 630}}));
        QCOMPARE(row.value("leading").toString(), "09:15\n10:30"); QCOMPARE(row.value("detail").toString(), "Room 2");
        QCOMPARE(row.value("accent").toString(), "#ff6347");
        const auto action = snapshot.value("actions").toMap().value(row.value("actionId").toString()).toMap();
        QCOMPARE(action.value("date").toString(), "2026-11-10"); QCOMPARE(action.value("eventId").toString(), id);
        event = manager.event(id); event["colour"] = "#80abcdef"; QVERIFY(!manager.saveEvent(event).isEmpty());
        QCOMPARE(rows(manager.widgetSnapshot("2026-11-08T22:00:00Z", 2, palette)).first().toMap().value("accent").toString(), "#80abcdef");
        event = manager.event(id); event["colour"] = "theme:3"; QVERIFY(!manager.saveEvent(event).isEmpty());
        QCOMPARE(rows(manager.widgetSnapshot("2026-11-08T22:00:00Z", 3, palette)).first().toMap().value("accent"), palette.value("accent"));
        const auto previousLocale = QLocale(); const QLocale translatedLocale(QLocale::French, QLocale::France); QLocale::setDefault(translatedLocale);
        const auto translated = manager.widgetSnapshot("2026-11-08T22:00:00Z", 4, palette); QLocale::setDefault(previousLocale);
        QCOMPARE(frames(translated).first().toMap().value("title").toString(), "10 novembre");
    }
    void onlyOneDayIsShownAndTodayFinishingAdvancesToNextRelevantDay() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(!add(manager, "Today", "2026-11-09T09:00:00Z", "2026-11-09T10:00:00Z").isEmpty());
        QVERIFY(!add(manager, "Tomorrow all day", "2026-11-10", "2026-11-11", true).isEmpty());
        QVERIFY(!add(manager, "Seventh day", "2026-11-15T09:00:00Z", "2026-11-15T10:00:00Z").isEmpty());
        QVERIFY(!add(manager, "Outside", "2026-11-16T09:00:00Z", "2026-11-16T10:00:00Z").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 1); QCOMPARE(rows(snapshot).first().toMap().value("title").toString(), "Today");
        const auto finished = at(snapshot, "2026-11-09T10:00:00Z");
        QCOMPARE(finished.value("rows").toList().size(), 1); const auto next = finished.value("rows").toList().first().toMap();
        QCOMPARE(next.value("title").toString(), "Tomorrow all day"); QCOMPARE(next.value("allDay").toBool(), true); QVERIFY(!next.contains("timeRange"));
        QCOMPARE(finished.value("timeGrid").toMap().value("startMinute").toInt(), 540);
        QCOMPARE(snapshot.value("actions").toMap().value(next.value("actionId").toString()).toMap().value("date").toString(), "2026-11-10");
        QCOMPARE(header(snapshot, "2026-11-09T08:00:00Z"), (QVariantMap{{"kind", "period"}, {"date", "2026-11-09"}, {"minute", 540}}));
        QCOMPARE(header(snapshot, "2026-11-09T10:00:00Z"), (QVariantMap{{"kind", "period"}, {"date", "2026-11-10"}, {"minute", 540}}));
        QVERIFY(frames(snapshot).first().toMap().value("actionId") != finished.value("actionId"));
        const auto afterAllDay = at(snapshot, "2026-11-11T00:00:00Z");
        QCOMPARE(afterAllDay.value("rows").toList().size(), 1); QCOMPARE(afterAllDay.value("rows").toList().first().toMap().value("title").toString(), "Seventh day");
        QTemporaryDir other; CalendarManager outside(other.path()); outside.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(!add(outside, "Outside", "2026-11-16T09:00:00Z", "2026-11-16T10:00:00Z").isEmpty());
        QVERIFY(rows(outside.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette)).isEmpty());
    }
    void ongoingRangesClipAtMidnightAndLastFrameIncludesFullLookahead() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(!add(manager, "Ongoing", "2026-11-08T23:00:00Z", "2026-11-09T02:30:00Z").isEmpty());
        QVERIFY(!add(manager, "Last lookahead", "2026-12-14", "2026-12-15", true, "Pacific/Honolulu").isEmpty());
        QVERIFY(!add(manager, "Beyond last lookahead", "2026-12-15", "2026-12-16", true).isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-09T01:15:00Z", 1, palette); checkBounds(snapshot);
        const auto ongoing = rows(snapshot).first().toMap();
        QCOMPARE(ongoing.value("timeRange").toMap(), (QVariantMap{{"startMinute", 0}, {"endMinute", 150}}));
        QCOMPARE(frames(snapshot).first().toMap().value("timeGrid").toMap().value("startMinute").toInt(), 60);
        QCOMPARE(at(snapshot, "2026-11-09T02:00:00Z").value("timeGrid").toMap().value("startMinute").toInt(), 120);
        QCOMPARE(snapshot.value("actions").toMap().value(ongoing.value("actionId").toString()).toMap().value("date").toString(), "2026-11-09");
        const auto last = at(snapshot, "2026-12-08T00:00:00Z").value("rows").toList();
        QCOMPARE(last.size(), 1); QCOMPARE(last.first().toMap().value("title").toString(), "Last lookahead");
        QCOMPARE(snapshot.value("actions").toMap().value(last.first().toMap().value("actionId").toString()).toMap().value("date").toString(), "2026-12-14");
        QCOMPARE(snapshot.value("validThrough").toString(), "2026-12-09T00:00:00Z");
        QTemporaryDir laterDirectory; CalendarManager later(laterDirectory.path()); later.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(!add(later, "To midnight", "2026-11-09T23:00:00Z", "2026-11-10T02:00:00Z").isEmpty());
        const auto late = later.widgetSnapshot("2026-11-09T21:00:00Z", 1, palette);
        QCOMPARE(rows(late).first().toMap().value("timeRange").toMap(), (QVariantMap{{"startMinute", 1380}, {"endMinute", 1440}}));
    }
    void timeGridPreservesSpringDstWallMinutesAndOverlappingEvents() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Europe/London"}});
        QVERIFY(!add(manager, "DST", "2026-03-29T00:30:00Z", "2026-03-29T02:30:00+01:00", false, "Europe/London").isEmpty());
        QVERIFY(!add(manager, "Overlap", "2026-03-29T00:45:00Z", "2026-03-29T03:00:00+01:00", false, "Europe/London").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-03-28T22:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 2);
        QCOMPARE(rows(snapshot)[0].toMap().value("timeRange").toMap(), (QVariantMap{{"startMinute", 30}, {"endMinute", 150}}));
        QCOMPARE(rows(snapshot)[1].toMap().value("timeRange").toMap(), (QVariantMap{{"startMinute", 45}, {"endMinute", 180}}));
    }
    void repeatedDstWallTimeKeepsActualLabelsWithAValidMinimumRange() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Europe/London"}});
        QVERIFY(!add(manager, "Repeated hour", "2026-10-25T01:45:00+01:00", "2026-10-25T01:15:00Z", false, "Europe/London").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-10-24T23:30:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).first().toMap().value("timeRange").toMap(), (QVariantMap{{"startMinute", 105}, {"endMinute", 106}}));
        QCOMPARE(rows(snapshot).first().toMap().value("leading").toString(), "01:45\n01:15");
    }
    void emptyAgendaHasTruthfulCoverageAndLogicalOpen() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        const auto snapshot = manager.widgetSnapshot("2026-11-09T12:00:00Z", 1, palette);
        checkBounds(snapshot); QCOMPARE(snapshot.value("palette").toMap(), palette);
        QCOMPARE(snapshot.value("validFrom").toString(), "2026-11-09T00:00:00Z");
        QCOMPARE(snapshot.value("validThrough").toString(), "2026-12-09T00:00:00Z");
        QCOMPARE(frames(snapshot).size(), 30); QVERIFY(rows(snapshot).isEmpty());
        QVERIFY(snapshot.value("actions").toMap().contains("open"));
        QCOMPARE(snapshot.value("actions").toMap().value("open").toMap(), (QVariantMap{{"kind", "period"}, {"date", "2026-11-09"}, {"minute", 540}}));
        QCOMPARE(manager.widgetToday("2026-11-10T00:01:00Z"), "2026-11-10");
    }
    void configuredZoneDstAndAllDayDates() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Europe/London"}});
        QVERIFY(!add(manager, "DST", "2026-03-28T09:00:00Z", "2026-03-28T10:00:00Z", false, "Europe/London", "FREQ=DAILY;COUNT=3").isEmpty());
        QVERIFY(!add(manager, "All day", "2026-03-29", "2026-03-30", true, "Pacific/Honolulu").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-03-28T07:00:00Z", 1, palette); checkBounds(snapshot);
        const auto next = onDay(snapshot, "2026-03-29T07:00:00Z", "2026-03-29"); QCOMPARE(next.size(), 2);
        const auto later = at(snapshot, "2026-03-30T07:00:00Z").value("rows").toList(); QCOMPARE(later.size(), 1);
        const auto recurrence = next[0].toMap().value("title") == "DST" ? next[0].toMap() : next[1].toMap();
        const auto data = snapshot.value("actions").toMap().value(recurrence.value("actionId").toString()).toMap();
        QCOMPARE(data.value("occurrence").toString(), "2026-03-29T08:00:00Z");
        QVERIFY(recurrence.value("leading").toString().contains("09:00"));
        QCOMPARE(manager.widgetToday("2026-03-29T23:30:00Z"), "2026-03-30");
        const auto starts = frames(snapshot); bool shortDay = false;
        for (const auto &frame : starts) if (frame.toMap().value("at") == "2026-03-29T23:00:00Z") shortDay = true;
        QVERIFY(shortDay);
    }
    void overnightAndAlreadyFinishedEvents() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(!add(manager, "Overnight", "2026-11-08T23:00:00Z", "2026-11-09T02:00:00Z").isEmpty());
        QVERIFY(!add(manager, "Finished", "2026-11-09T00:00:00Z", "2026-11-09T00:30:00Z").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-09T01:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 1); QCOMPARE(rows(snapshot)[0].toMap().value("title").toString(), "Overnight");
        QVERIFY(at(snapshot, "2026-11-09T02:00:00Z").value("rows").toList().isEmpty());
        const auto data = snapshot.value("actions").toMap().value(rows(snapshot)[0].toMap().value("actionId").toString()).toMap();
        QCOMPARE(data.value("date").toString(), "2026-11-09");
    }
    void allDayDateIsIndependentOfOppositeSourceZone() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "Pacific/Kiritimati"}});
        QVERIFY(!add(manager, "Date only", "2026-11-09", "2026-11-10", true, "Pacific/Honolulu").isEmpty());
        QVERIFY(manager.events("2026-11-09", "2026-11-10").isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-08T11:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 1); QCOMPARE(rows(snapshot)[0].toMap().value("title").toString(), "Date only");
        QVERIFY(at(snapshot, "2026-11-09T10:00:00Z").value("rows").toList().isEmpty());
        const auto data = snapshot.value("actions").toMap().value(rows(snapshot)[0].toMap().value("actionId").toString()).toMap();
        QCOMPARE(data.value("occurrence").toString(), "2026-11-09"); QCOMPARE(data.value("date").toString(), "2026-11-09");
    }
    void recurrenceExceptionsUseExpandedCurrentEvents() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        const auto id = add(manager, "Series", "2026-11-09T09:00:00Z", "2026-11-09T10:00:00Z", false, "UTC", "FREQ=DAILY;COUNT=3"); QVERIFY(!id.isEmpty());
        auto exception = manager.event(id); exception["title"] = "Moved instance";
        exception["start"] = "2026-11-10T11:00:00Z"; exception["end"] = "2026-11-10T12:00:00Z";
        QVERIFY(!manager.saveEvent(exception, "one", "2026-11-10T09:00:00Z").isEmpty());
        QVERIFY(manager.deleteEvent(id, "one", "2026-11-11T09:00:00Z"));
        const auto snapshot = manager.widgetSnapshot("2026-11-09T07:00:00Z", 1, palette); checkBounds(snapshot);
        const auto next = at(snapshot, "2026-11-10T07:00:00Z").value("rows").toList(); QCOMPARE(next.size(), 1);
        QCOMPARE(next[0].toMap().value("title").toString(), "Moved instance");
        QVERIFY(at(snapshot, "2026-11-11T07:00:00Z").value("rows").toList().isEmpty());
    }
    void crowdingHasReadableOverflowAndBoundedCoverage() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        const auto start = QDateTime::fromString("2026-11-09T09:00:00Z", Qt::ISODate);
        for (int i = 0; i < 70; ++i) QVERIFY(!add(manager, QString("Event %1").arg(i), start.toString(Qt::ISODate), start.addSecs(60 * (i + 1)).toString(Qt::ISODate)).isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 64); QCOMPARE(rows(snapshot).last().toMap().value("id").toString(), "overflow");
        QVERIFY(rows(snapshot).last().toMap().value("title").toString().contains("7"));
        QVERIFY(!rows(snapshot).last().toMap().contains("timeRange")); QVERIFY(!rows(snapshot).last().toMap().contains("allDay"));
        QVERIFY(snapshot.value("validThrough").toString() < "2026-11-10T00:00:00Z");
        const auto later = manager.widgetSnapshot("2026-11-09T10:08:00Z", 2, palette); checkBounds(later); QCOMPARE(rows(later).size(), 2);
    }
    void unicodePlainTextStableActionsAndPayloadBudget() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        const QString title = QString::fromUcs4(U"📅").repeated(300) + "\n\t";
        const auto id = add(manager, title, "2026-11-09T09:00:00Z", "2026-11-09T10:00:00Z", false, "UTC", "FREQ=DAILY;COUNT=30"); QVERIFY(!id.isEmpty());
        auto event = manager.event(id); event["location"] = QString(3000, QChar(0x754c)) + "\nRoom"; QVERIFY(!manager.saveEvent(event).isEmpty());
        const auto one = manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette); checkBounds(one);
        const auto two = manager.widgetSnapshot("2026-11-09T08:01:00Z", 2, palette); checkBounds(two);
        QCOMPARE(rows(one).size(), 1); QCOMPARE(rows(two).size(), 1);
        QCOMPARE(onDay(one, "2026-11-09T08:00:00Z", "2026-11-09").size(), 1);
        QCOMPARE(rows(one)[0].toMap().value("actionId"), rows(two)[0].toMap().value("actionId"));
        QCOMPARE(rows(one)[0].toMap().value("title").toString().toUcs4().size(), 256);
        QVERIFY(!rows(one)[0].toMap().value("title").toString().contains('\n'));
        QVERIFY(rows(one)[0].toMap().value("detail").toString().toUcs4().size() <= 512);
    }
    void invalidAndInactiveNeverPublish() {
        QTemporaryDir directory; CalendarManager manager(directory.path()); manager.setPreferences({{"timeZone", "UTC"}});
        QVERIFY(manager.widgetSnapshot("invalid", 1, palette).isEmpty());
        for (const auto generation : {0.0, -1.0, 1.5, 9007199254740992.0, std::numeric_limits<double>::infinity()})
            QVERIFY(manager.widgetSnapshot("2026-11-09T08:00:00Z", generation, palette).isEmpty());
        manager.setActive(false); QVERIFY(manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette).isEmpty());
        manager.setActive(true); QVERIFY(manager.beginGitUpdate());
        QVERIFY(manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette).isEmpty()); QVERIFY(manager.finishGitUpdate());
    }
    void payloadBudgetShortensCoverageInsteadOfTruncatingRows() {
        QTemporaryDir directory; CalendarManager manager(directory.path() + "/calendar"); manager.setPreferences({{"timeZone", "UTC"}});
        for (int i = 0; i < 8; ++i) {
            const auto id = add(manager, QString::fromUcs4(U"📅").repeated(256), "2026-11-09T09:00:00Z", "2026-11-09T10:00:00Z", false, "UTC", "FREQ=DAILY;COUNT=30");
            QVERIFY(!id.isEmpty()); auto event = manager.event(id); event["location"] = QString(512, QChar(0x754c));
            QVERIFY(!manager.saveEvent(event).isEmpty());
        }
        const auto snapshot = manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 8); QVERIFY(frames(snapshot).size() < 60);
        QVERIFY(QDateTime::fromString(snapshot.value("validThrough").toString(), Qt::ISODate) < QDateTime::fromString("2026-12-09T00:00:00Z", Qt::ISODate));
        WidgetStore store(directory.path() + "/widgets");
        QVERIFY(store.configure(directory.path(), {QVariantMap{{"addonId", "calendar"}, {"widgetId", "agenda"}, {"name", "Agenda"}}}));
        QVERIFY2(store.publish("calendar", "agenda", snapshot), qPrintable(store.error()));
    }
    void coreAcceptsSnapshotAndResolvesCurrentOwnedActions() {
        QTemporaryDir directory; CalendarManager manager(directory.path() + "/calendar"); manager.setPreferences({{"timeZone", "UTC"}});
        const auto id = add(manager, "Owned", "2026-11-09T09:00:00Z", "2026-11-09T10:00:00Z"); QVERIFY(!id.isEmpty());
        const auto snapshot = manager.widgetSnapshot("2026-11-09T08:00:00Z", 1, palette); checkBounds(snapshot);
        QCOMPARE(rows(snapshot).size(), 1);
        WidgetStore store(directory.path() + "/widgets");
        QVERIFY(store.configure(directory.path(), {QVariantMap{{"addonId", "calendar"}, {"widgetId", "agenda"}, {"name", "Agenda"}}}));
        QVERIFY2(store.publish("calendar", "agenda", snapshot), qPrintable(store.error()));
        QUrl url("pilon://widget/calendar/agenda"); QUrlQuery query;
        query.addQueryItem("workspace", store.workspaceId()); query.addQueryItem("action", rows(snapshot)[0].toMap().value("actionId").toString());
        query.addQueryItem("generation", "1"); url.setQuery(query);
        const auto resolved = store.resolve(url, QDateTime::fromString("2026-11-09T08:00:00Z", Qt::ISODate).toMSecsSinceEpoch());
        QCOMPARE(resolved.value("data").toMap().value("eventId").toString(), id);
    }
};
int main(int argc, char **argv) {
    QTemporaryDir profile; qputenv("XDG_CONFIG_HOME", profile.path().toUtf8()); qputenv("XDG_DATA_HOME", profile.path().toUtf8());
    QCoreApplication app(argc, argv); app.setOrganizationName("PilonTests"); app.setApplicationName("CalendarWidgets");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    CalendarWidgetTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "calendar_widget_tests.moc"
