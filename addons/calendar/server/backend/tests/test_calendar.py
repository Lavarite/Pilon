import copy
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import sqlite3
from pathlib import Path
import tempfile
import unittest

from app.calendar import Store, occurrences, project


def publication():
    return {"fields": ["title"], "from": "2026-10-01", "through": "2026-10-31",
            "preferences": {"timeZone": "Europe/London", "firstDay": 1, "view": 2},
            "events": [{"id": "lecture", "title": "Lecture", "location": "Private room", "description": "Private notes",
                        "guests": [{"email": "private@example.com"}], "account": "private-account",
                        "start": "2026-10-02T09:00:00+01:00", "end": "2026-10-02T10:00:00+01:00", "timeZone": "Europe/London"}]}


class CalendarTests(unittest.TestCase):
    def test_publication_preserves_instants_across_a_repeated_hour(self):
        payload = publication()
        event = payload["events"][0]
        event.update(start="2026-10-25T01:50:00+01:00", end="2026-10-25T01:10:00+00:00")
        for fields in (["title"], []):
            with self.subTest(fields=fields):
                payload["fields"] = fields
                result = occurrences(project(payload), "2026-10-25", "2026-10-26")
                self.assertEqual(len(result), 1)
                self.assertEqual(result[0]["endMs"] - result[0]["startMs"], 20 * 60 * 1000)
        with self.assertRaises(ValueError):
            event["start"], event["end"] = event["end"], event["start"]
            project(payload)

    def test_publication_timestamp_is_server_generated_and_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "calendar.sqlite"
            store = Store(path)
            payload = publication()
            payload["publishedAt"] = "1900-01-01T00:00:00Z"
            before = datetime.now(timezone.utc)
            token = store.create(payload)
            saved = Store(path).get(token)
            published = datetime.fromisoformat(saved["publishedAt"].replace("Z", "+00:00"))
            self.assertGreaterEqual(published, before)
            self.assertLessEqual(published, datetime.now(timezone.utc))
            self.assertTrue(store.update(token, publication()))
            updated = datetime.fromisoformat(store.get(token)["publishedAt"].replace("Z", "+00:00"))
            self.assertGreaterEqual(updated, published)

    def test_client_token_creation_replays_without_duplicate_or_timestamp_change(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "calendar.sqlite")
            token = "a" * 43
            self.assertEqual(store.create(publication(), token), token)
            first = store.get(token)
            self.assertEqual(store.create(publication(), token), token)
            self.assertEqual(store.get(token), first)
            changed = publication()
            changed["events"][0]["title"] = "Different intent"
            with self.assertRaises(ValueError):
                store.create(changed, token)
            self.assertEqual(store.get(token), first)

    def test_client_token_creation_rejects_invalid_tokens(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "calendar.sqlite")
            for token in ("short", "a" * 44, "/" * 43):
                with self.subTest(token=token), self.assertRaises(ValueError):
                    store.create(publication(), token)

    def test_revocation_prevents_a_delayed_pending_create_from_reviving_a_link(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "calendar.sqlite")
            token = "d" * 43
            store.create(publication(), token)
            self.assertTrue(store.delete(token))
            self.assertFalse(store.delete(token))
            with self.assertRaises(ValueError):
                store.create(publication(), token)
            self.assertIsNone(store.get(token))
            pending = "e" * 43
            self.assertFalse(store.delete(pending))
            with self.assertRaises(ValueError):
                store.create(publication(), pending)

    def test_concurrent_create_retries_share_one_durable_intent(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "calendar.sqlite")
            token = "f" * 43
            with ThreadPoolExecutor(max_workers=4) as workers:
                self.assertEqual(list(workers.map(lambda _: store.create(publication(), token), range(8))), [token] * 8)
            self.assertEqual(store.get(token)["events"][0]["title"], "Lecture")

    def test_legacy_rows_have_an_unknown_publication_time_until_updated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "calendar.sqlite"
            with sqlite3.connect(path) as db:
                db.execute("CREATE TABLE shares (token TEXT PRIMARY KEY, payload TEXT NOT NULL)")
                db.execute("INSERT INTO shares VALUES (?, ?)", (Store.digest("g" * 43), json.dumps(project(publication()))))
            store = Store(path)
            self.assertIsNone(store.get("g" * 43)["publishedAt"])
            self.assertTrue(store.update("g" * 43, publication()))
            self.assertTrue(store.get("g" * 43)["publishedAt"])

    def test_withheld_fields_never_reach_storage_or_reader(self):
        payload = project(publication())
        stored = json.dumps(payload)
        self.assertNotIn("Private", stored)
        self.assertNotIn("private@example.com", stored)
        self.assertNotIn("private-account", stored)
        events = occurrences(payload, "2026-10-01", "2026-11-01")
        self.assertEqual(events[0]["title"], "Lecture")
        self.assertNotIn("location", events[0])
        payload["fields"] = []
        payload["events"][0].pop("title")
        self.assertNotIn("title", occurrences(payload, "2026-10-01", "2026-11-01")[0])

    def test_range_cannot_be_extended_by_the_reader(self):
        payload = publication()
        payload["from"] = "2026-10-10"
        self.assertEqual(occurrences(project(payload), "2026-10-01", "2026-11-01"), [])
        self.assertEqual(occurrences(project(payload), "2026-09-01", "2026-10-01"), [])
        with self.assertRaises(ValueError):
            occurrences(project(payload), "2026-01-01", "2027-01-01")

    def test_hidden_titles_combine_consecutive_and_overlapping_events_into_plain_blocks(self):
        payload = publication(); payload["fields"] = []
        original = payload["events"][0]
        payload["events"] = [dict(original, id=str(i), colour=f"theme:{i}",
                                  start=f"2026-10-02T{start}:00+01:00", end=f"2026-10-02T{end}:00+01:00")
                             for i, (start, end) in enumerate([("11:00", "12:00"), ("12:00", "13:00"),
                                                              ("12:30", "16:00"), ("17:00", "18:00")])]
        events = occurrences(project(payload), "2026-10-01", "2026-11-01")
        self.assertEqual(len(events), 2)
        self.assertEqual([(e["startMinute"], e["endMinute"]) for e in events], [(660, 960), (1020, 1080)])
        for event in events:
            for key in ("title", "colour", "location", "description", "guests"):
                self.assertNotIn(key, event)
        self.assertNotIn("Busy", json.dumps(events))
        payload["fields"] = ["title"]
        self.assertEqual(len(occurrences(project(payload), "2026-10-01", "2026-11-01")), 4)

    def test_merged_blocks_keep_only_the_selected_details(self):
        payload = publication(); payload["fields"] = ["description", "url", "guests"]
        one = payload["events"][0]
        one.update(url="https://example.com/one", description="First note")
        two = dict(one, id="two", start="2026-10-02T10:00:00+01:00", end="2026-10-02T11:00:00+01:00",
                   url="https://example.com/two", description="Second note")
        payload["events"].append(two)
        event = occurrences(project(payload), "2026-10-01", "2026-11-01")[0]
        self.assertEqual(event["description"], "First note\nSecond note")
        self.assertEqual(event["urls"], [one["url"], two["url"]])
        self.assertEqual(event["guests"], one["guests"])
        self.assertNotIn("title", event); self.assertNotIn("location", event)

    def test_hidden_blocks_join_across_midnight_without_filling_gaps(self):
        payload = publication(); payload["fields"] = []
        one = payload["events"][0]
        one.update(start="2026-10-02T23:00:00+01:00", end="2026-10-03T01:00:00+01:00")
        payload["events"].append(dict(one, id="two", start="2026-10-03T01:00:00+01:00", end="2026-10-03T02:00:00+01:00"))
        event = occurrences(project(payload), "2026-10-01", "2026-11-01")[0]
        self.assertEqual(event["displayDate"], "2026-10-02")
        self.assertEqual(event["displayEndDate"], "2026-10-03")
        self.assertEqual((event["startMinute"], event["endMinute"]), (1380, 120))

    def test_hidden_blocks_use_elapsed_times_across_the_repeated_daylight_hour(self):
        payload = publication(); payload["fields"] = []
        one = payload["events"][0]
        one.update(start="2026-10-25T01:00:00+01:00", end="2026-10-25T01:30:00+01:00")
        payload["events"].append(dict(one, id="two", start="2026-10-25T01:00:00Z", end="2026-10-25T01:30:00Z"))
        self.assertEqual(len(occurrences(project(payload), "2026-10-01", "2026-11-01")), 2)

    def test_recurrence_timezone_exceptions_and_additional_dates(self):
        payload = publication()
        event = payload["events"][0]
        event.update(start="2026-10-23T09:00:00+01:00", end="2026-10-23T10:00:00+01:00",
                     recurrence="FREQ=DAILY;COUNT=5", exdates=["2026-10-24T09:00:00+01:00"],
                     rdates=["2026-10-30T09:00:00Z"])
        events = occurrences(project(payload), "2026-10-01", "2026-11-01")
        self.assertEqual([e["displayDate"] for e in events], ["2026-10-23", "2026-10-25", "2026-10-26", "2026-10-27", "2026-10-30"])
        self.assertTrue(all(e["startMinute"] == 540 for e in events))
        self.assertEqual(events[0]["start"][11:16], "09:00")
        self.assertTrue(events[0]["start"].endswith("+01:00"))
        self.assertTrue(events[1]["start"].endswith("+00:00"))

    def test_events_crossing_range_boundaries_do_not_expose_times_outside_it(self):
        payload = publication(); payload["from"] = "2026-10-02"; payload["through"] = "2026-10-02"
        payload["events"][0].update(start="2026-10-01T18:00:00+01:00", end="2026-10-03T09:00:00+01:00")
        event = occurrences(project(payload), "2026-10-01", "2026-11-01")[0]
        self.assertEqual(event["start"], "2026-10-02T00:00:00+01:00")
        self.assertEqual(event["end"], "2026-10-03T00:00:00+01:00")

    def test_provider_instances_preserve_changes_and_cancellations_without_private_fields(self):
        payload = publication()
        event = payload["events"][0]
        event["recurrence"] = "FREQ=DAILY;COUNT=3"
        changed = dict(event, title="Rescheduled lecture", start="2026-10-02T11:00:00+01:00", end="2026-10-02T12:00:00+01:00")
        changed.pop("recurrence")
        event["instances"] = {"from": "2026-10-01", "to": "2026-11-01", "events": [changed]}
        payload = project(payload)
        self.assertNotIn("Private", json.dumps(payload))
        events = occurrences(payload, "2026-10-01", "2026-11-01")
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["title"], "Rescheduled lecture")
        self.assertEqual(events[0]["startMinute"], 660)
        wider = occurrences(payload, "2026-09-30", "2026-11-02")
        self.assertEqual([(e["displayDate"], e["startMinute"]) for e in wider], [("2026-10-02", 660)])
        payload["events"][0]["instances"].update({"from": "2026-10-02", "to": "2026-10-04"})
        partial = occurrences(payload, "2026-10-01", "2026-10-06")
        self.assertEqual([(e["displayDate"], e["startMinute"]) for e in partial],
                         [("2026-10-02", 660), ("2026-10-04", 540)])

    def test_all_day_end_is_exclusive_and_until_is_inclusive(self):
        payload = publication()
        payload["events"][0].update(start="2026-10-02", end="2026-10-04", allDay=True,
                                     recurrence="FREQ=DAILY;UNTIL=20261004")
        events = occurrences(project(payload), "2026-10-01", "2026-10-10")
        self.assertEqual(len(events), 3)
        self.assertEqual(events[0]["displayEndDate"], "2026-10-04")
        self.assertEqual(events[0]["endMinute"], 0)

    def test_link_update_revocation_and_persistence(self):
        with tempfile.TemporaryDirectory() as directory:
            store = Store(Path(directory) / "calendar.sqlite")
            token = store.create(publication())
            self.assertEqual(len(token), 43)
            other = Store(Path(directory) / "calendar.sqlite")
            self.assertEqual(other.get(token)["fields"], ["title"])
            changed = publication(); changed["fields"] = ["location"]
            self.assertTrue(other.update(token, changed))
            self.assertNotIn("title", store.get(token)["events"][0])
            self.assertEqual(store.get(token)["events"][0]["location"], "Private room")
            self.assertTrue(store.delete(token))
            self.assertIsNone(other.get(token))


if __name__ == "__main__":
    unittest.main()
