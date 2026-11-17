"""Store and render only the calendar data the owner chose to publish."""
from contextlib import contextmanager
from datetime import date, datetime, time, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
import secrets
import sqlite3
from zoneinfo import ZoneInfo

from dateutil.rrule import rrulestr

DETAILS = {"title", "location", "description", "url", "guests"}
EVENT_KEYS = {"id", "start", "end", "allDay", "timeZone", "recurrence", "exdates", "rdates", "colour"}
TOKEN = re.compile(r"^[A-Za-z0-9_-]{43}$")


class PublicationConflict(ValueError):
    """An existing read token belongs to a different publication intent."""


def moment(value, zone):
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    return parsed.replace(tzinfo=zone) if parsed.tzinfo is None else parsed.astimezone(zone)


def project(payload):
    """Whitelist twice: withheld fields are neither stored nor sent to a reader."""
    fields = sorted(set(payload.get("fields", [])) & DETAILS)
    first, last = date.fromisoformat(payload["from"]), date.fromisoformat(payload["through"])
    if last < first:
        raise ValueError("Invalid sharing range")
    source = payload.get("preferences", {})
    preferences = {"timeZone": source.get("timeZone", "UTC"), "firstDay": source.get("firstDay", 1),
                   "view": source.get("view", 2), "eventLayout": source.get("eventLayout", "separate")}
    ZoneInfo(preferences["timeZone"])
    if preferences["firstDay"] not in range(1, 8) or preferences["view"] not in range(3):
        raise ValueError("Invalid calendar preferences")
    events = payload.get("events", [])
    if not isinstance(events, list) or len(events) > 10000:
        raise ValueError("Too many events")
    result = []
    for event in events:
        item = {key: event[key] for key in EVENT_KEYS | set(fields) if key in event}
        zone = ZoneInfo(item.get("timeZone") or preferences["timeZone"])
        start, end = moment(item["start"], zone), moment(item["end"], zone)
        if end <= start or len(item.get("recurrence", "")) > 1024:
            raise ValueError("Invalid event dates")
        # Validate recurrence before accepting the publication.
        if item.get("recurrence"):
            recurrence(item["recurrence"], start)
        if "url" in item and not item["url"].startswith(("https://", "http://")):
            item.pop("url")
        if "guests" in item:
            item["guests"] = [{key: guest[key] for key in ("name", "email", "status") if key in guest}
                              for guest in item["guests"][:1000]]
        if event.get("instances"):
            cache = event["instances"]
            if date.fromisoformat(cache["to"]) <= date.fromisoformat(cache["from"]):
                raise ValueError("Invalid instance range")
            cached_events = [{key: value for key, value in entry.items() if key != "instances"}
                             for entry in cache["events"]]
            clean = project({"fields": fields, "preferences": preferences, "from": first.isoformat(),
                             "through": last.isoformat(), "events": cached_events})
            item["instances"] = {"from": cache["from"], "to": cache["to"], "events": clean["events"]}
        result.append(item)
    return {"fields": fields, "preferences": preferences, "events": result,
            "from": first.isoformat(), "through": last.isoformat()}


def recurrence(rule, start):
    def until(match):
        value = match[1]
        if value.endswith("Z"):
            return match[0]
        parsed = datetime.strptime(value, "%Y%m%d" if len(value) == 8 else "%Y%m%dT%H%M%S")
        if len(value) == 8:
            parsed = datetime.combine(parsed.date(), time(23, 59, 59))
        return "UNTIL=" + parsed.replace(tzinfo=start.tzinfo).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return rrulestr(re.sub(r"UNTIL=([^;]+)", until, rule), dtstart=start)


def occurrences(payload, begin, end):
    display_zone = ZoneInfo(payload["preferences"]["timeZone"])
    lower = datetime.combine(date.fromisoformat(begin), time(), display_zone)
    upper = datetime.combine(date.fromisoformat(end), time(), display_zone)
    if not 0 < (upper.date() - lower.date()).days <= 62:
        raise ValueError("Choose a range of at most 62 days")
    permitted_start = datetime.combine(date.fromisoformat(payload["from"]), time(), display_zone)
    permitted_end = datetime.combine(date.fromisoformat(payload["through"]) + timedelta(days=1), time(), display_zone)
    lower = max(lower, permitted_start)
    upper = min(upper, permitted_end)
    if upper <= lower:
        return []
    result = []
    events = []
    for event in payload["events"]:
        cache = event.get("instances")
        # Match CalendarManager::events: the provider's expanded instances take
        # precedence only when its cache covers the requested view completely.
        if cache and cache["from"] <= begin and cache["to"] >= end:
            events.extend(cache["events"])
        else:
            events.append(event)
    for event in events:
        zone = ZoneInfo(event.get("timeZone") or display_zone.key)
        start, finish = moment(event["start"], zone), moment(event["end"], zone)
        all_day = event.get("allDay", False)
        duration = (finish - start) if all_day else finish.astimezone(timezone.utc) - start.astimezone(timezone.utc)
        excluded = {moment(value, zone).astimezone(timezone.utc) for value in event.get("exdates", [])}
        emitted = set()
        if event.get("recurrence"):
            iterator = recurrence(event["recurrence"], start).xafter((lower - duration).astimezone(zone), inc=True)
        else:
            iterator = iter([start])
        def add(at):
            at_utc = at.astimezone(timezone.utc)
            stop = at + duration if all_day else (at_utc + duration).astimezone(zone)
            if stop <= lower or at >= upper or at_utc in excluded or at_utc in emitted:
                return
            emitted.add(at_utc)
            first = max(at if all_day else at.astimezone(display_zone), permitted_start)
            last = min(stop if all_day else stop.astimezone(display_zone), permitted_end)
            item = {key: event[key] for key in DETAILS | {"id", "colour"} if key in event}
            item.update(allDay=all_day,
                        start=first.isoformat(), end=last.isoformat(), timeZone=display_zone.key,
                        displayDate=first.date().isoformat(), displayEndDate=last.date().isoformat(),
                        startMinute=first.hour * 60 + first.minute, endMinute=last.hour * 60 + last.minute,
                        startMs=int(first.timestamp() * 1000), endMs=int(last.timestamp() * 1000),
                        occurrence=first.date().isoformat() if all_day else first.astimezone(timezone.utc).isoformat())
            result.append(item)
            if len(result) > 10000:
                raise ValueError("Too many occurrences in this range")
        for at in iterator:
            if at >= upper:
                break
            add(at)
        for value in event.get("rdates", []):
            add(moment(value, zone))
    result.sort(key=lambda event: event["startMs"])
    return result if "title" in payload["fields"] else continuous_blocks(result, payload["fields"], display_zone)


def continuous_blocks(events, fields, zone):
    """Hide the boundaries and colours of individual undisclosed events."""
    intervals = []
    for event in events:
        if event["allDay"]:
            first = datetime.combine(date.fromisoformat(event["displayDate"]), time(), zone)
            last = datetime.combine(date.fromisoformat(event["displayEndDate"]), time(), zone)
        else:
            first, last = moment(event["start"], zone), moment(event["end"], zone)
        intervals.append((first, last, event))
    blocks = []
    for first, last, event in sorted(intervals, key=lambda value: value[0].timestamp()):
        if not blocks or first.timestamp() > blocks[-1]["end"].timestamp():
            blocks.append({"start": first, "end": last, "details": {field: [] for field in fields}})
        block = blocks[-1]
        if last.timestamp() > block["end"].timestamp():
            block["end"] = last
        for field in fields:
            values = event.get("guests", []) if field == "guests" else [event.get(field)]
            for value in values:
                if value and value not in block["details"][field]:
                    block["details"][field].append(value)
    result = []
    for block in blocks:
        first, last = block["start"], block["end"]
        identity = first.isoformat() + "/" + last.isoformat()
        item = {"id": hashlib.sha256(identity.encode()).hexdigest(), "occurrence": first.isoformat(),
                "allDay": first.time() == time() and last.time() == time(), "timeZone": zone.key,
                "start": first.isoformat(), "end": last.isoformat(),
                "displayDate": first.date().isoformat(), "displayEndDate": last.date().isoformat(),
                "startMinute": first.hour * 60 + first.minute, "endMinute": last.hour * 60 + last.minute,
                "startMs": int(first.timestamp() * 1000), "endMs": int(last.timestamp() * 1000)}
        for field, values in block["details"].items():
            if field == "guests": item[field] = values
            elif field == "url": item["urls"] = values
            else: item[field] = "\n".join(values)
        result.append(item)
    return result


class Store:
    def __init__(self, path):
        self.path = str(path)
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.execute("CREATE TABLE IF NOT EXISTS shares (token TEXT PRIMARY KEY, payload TEXT NOT NULL)")
            db.execute("CREATE TABLE IF NOT EXISTS revoked (token TEXT PRIMARY KEY)")
        Path(path).chmod(0o600)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def digest(token):
        return hashlib.sha256(token.encode()).hexdigest()

    def create(self, payload, token=None):
        token = secrets.token_urlsafe(32) if token is None else token
        if not TOKEN.fullmatch(token):
            raise ValueError("Invalid read token")
        clean = project(payload)
        stamped = dict(clean, publishedAt=datetime.now(timezone.utc).isoformat().replace("+00:00", "Z"))
        with self.connect() as db:
            digest = self.digest(token)
            inserted = db.execute("INSERT OR IGNORE INTO shares SELECT ?, ? WHERE NOT EXISTS "
                                  "(SELECT 1 FROM revoked WHERE token=?)",
                                  (digest, json.dumps(stamped), digest)).rowcount
            if not inserted:
                row = db.execute("SELECT payload FROM shares WHERE token=?", (digest,)).fetchone()
                if row is None:
                    raise PublicationConflict("Read token has been revoked")
                existing = json.loads(row[0])
                existing.pop("publishedAt", None)
                if existing != clean:
                    raise PublicationConflict("Read token already belongs to another publication")
        return token

    def get(self, token):
        if not TOKEN.fullmatch(token):
            return None
        with self.connect() as db:
            row = db.execute("SELECT payload FROM shares WHERE token=?", (self.digest(token),)).fetchone()
        if not row:
            return None
        payload = json.loads(row[0])
        payload.setdefault("publishedAt", None)
        return payload

    def update(self, token, payload):
        clean = project(payload)
        clean["publishedAt"] = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
        with self.connect() as db:
            return bool(db.execute("UPDATE shares SET payload=? WHERE token=?",
                                   (json.dumps(clean), self.digest(token))).rowcount)

    def delete(self, token):
        with self.connect() as db:
            digest = self.digest(token)
            db.execute("INSERT OR IGNORE INTO revoked VALUES (?)", (digest,))
            return bool(db.execute("DELETE FROM shares WHERE token=?", (digest,)).rowcount)
