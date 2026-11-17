package org.pilon;

import java.io.File;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.time.LocalDateTime;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

/** Reads only the bounded, derived widget format. No Qt, account or network access. */
final class PilonWidgetData {
    static final int MAX_BYTES = 256 * 1024;
    static final long MAX_GENERATION = 9007199254740991L;
    private PilonWidgetData() {}

    static final class Provider {
        final String addonId, widgetId, name;
        Provider(String addonId, String widgetId, String name) { this.addonId = addonId; this.widgetId = widgetId; this.name = name; }
    }
    static final class Catalog {
        final String workspaceId;
        final List<Provider> providers;
        Catalog(String workspaceId, List<Provider> providers) { this.workspaceId = workspaceId; this.providers = providers; }
        Provider find(String addonId, String widgetId) {
            for (Provider provider : providers) if (provider.addonId.equals(addonId) && provider.widgetId.equals(widgetId)) return provider;
            return null;
        }
    }
    static final class Snapshot {
        final JSONObject value;
        final long generation, validFrom, validThrough;
        Snapshot(JSONObject value, long generation, long from, long through) {
            this.value = value; this.generation = generation; validFrom = from; validThrough = through;
        }
        JSONObject frame(long now) {
            if (now < validFrom || now >= validThrough) return null;
            JSONArray frames = value.optJSONArray("timeline"); JSONObject current = null;
            for (int index = 0; index < frames.length(); ++index) {
                JSONObject frame = frames.optJSONObject(index);
                if (date(frame.opt("at")) > now) break;
                current = frame;
            }
            return current;
        }
    }
    static boolean addonId(String value) {
        return value != null && value.length() <= 128 && (Arrays.asList("files", "ssh", "assistant", "notes", "calendar", "git", "devices").contains(value)
            || value.matches("[a-z][a-z0-9-]*(\\.[a-z][a-z0-9-]*)+"));
    }
    static boolean widgetId(String value) { return value != null && value.matches("[a-z][a-z0-9-]{0,63}"); }
    static boolean workspaceId(String value) { return value != null && value.matches("[a-f0-9]{64}"); }
    private static boolean id(String value) { return value != null && value.matches("[A-Za-z0-9_-]{1,128}"); }
    private static boolean text(Object value, int maximum, boolean newlines) {
        if (!(value instanceof String)) return false;
        String string = (String)value;
        if (string.codePointCount(0, string.length()) > maximum) return false;
        for (int index = 0; index < string.length(); ++index) {
            char c = string.charAt(index);
            if (Character.isHighSurrogate(c)) {
                if (++index == string.length() || !Character.isLowSurrogate(string.charAt(index))) return false;
            } else if (Character.isLowSurrogate(c)) return false;
            if (Character.getType(c) == Character.CONTROL && !(newlines && c == '\n')) return false;
        }
        return true;
    }
    private static boolean blank(String value) {
        for (int index = 0; index < value.length();) {
            int point = value.codePointAt(index); index += Character.charCount(point);
            if (!Character.isWhitespace(point) && !Character.isSpaceChar(point) && point != 0x85) return false;
        }
        return true;
    }
    private static void require(boolean value) { if (!value) throw new IllegalArgumentException("Invalid widget data"); }
    private static boolean keys(JSONObject object, String... names) {
        return object != null && object.length() == names.length && new HashSet<>(Arrays.asList(names)).containsAll(keySet(object));
    }
    private static Set<String> keySet(JSONObject object) {
        Set<String> result = new HashSet<>(); Iterator<String> keys = object.keys(); while (keys.hasNext()) result.add(keys.next()); return result;
    }
    private static long integer(Object value, long maximum) {
        require(value instanceof Number); double number = ((Number)value).doubleValue();
        require(Double.isFinite(number) && number == Math.floor(number) && number >= 1 && number <= maximum); return (long)number;
    }
    private static boolean interval(JSONObject value) {
        if (!keys(value, "startMinute", "endMinute")) return false;
        Object start = value.opt("startMinute"), end = value.opt("endMinute");
        if (!(start instanceof Number) || !(end instanceof Number)) return false;
        double a = ((Number)start).doubleValue(), b = ((Number)end).doubleValue();
        return Double.isFinite(a) && Double.isFinite(b) && a == Math.floor(a) && b == Math.floor(b) && a >= 0 && a < b && b <= 1440;
    }
    static long date(Object value) {
        try {
            if (!(value instanceof String) || !((String)value).matches("[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,3})?Z")) return Long.MIN_VALUE;
            String string = (String)value; if (string.startsWith("0000")) return Long.MIN_VALUE;
            return LocalDateTime.parse(string.substring(0, string.length() - 1), DateTimeFormatter.ISO_LOCAL_DATE_TIME).toInstant(ZoneOffset.UTC).toEpochMilli();
        } catch (RuntimeException failure) { return Long.MIN_VALUE; }
    }
    static String filename(String addonId, String widgetId) throws Exception {
        require(addonId(addonId) && widgetId(widgetId));
        byte[] bytes = MessageDigest.getInstance("SHA-256").digest((addonId + "/" + widgetId).getBytes(StandardCharsets.UTF_8));
        StringBuilder result = new StringBuilder(69); for (byte value : bytes) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255)); return result + ".json";
    }
    static int newlyPinnedId(int supplied, int[] before, int[] current) {
        Set<Integer> old = new HashSet<>(), added = new HashSet<>();
        for (int id : before) old.add(id);
        for (int id : current) if (id > 0 && !old.contains(id)) added.add(id);
        if (added.contains(supplied)) return supplied;
        return added.size() == 1 ? added.iterator().next() : 0;
    }
    private static JSONObject read(File root, String filename) throws Exception {
        String json = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(PilonWidgetFiles.bytes(root, filename, MAX_BYTES))).toString();
        return parse(json);
    }
    static JSONObject parse(String json) throws Exception {
        require(json != null && json.length() <= MAX_BYTES); new JsonBounds(json).check(); return new JSONObject(json);
    }
    static Catalog catalog(File root) {
        try {
            JSONObject value = read(root, "catalog.json"); require(keys(value, "version", "workspaceId", "providers"));
            require(integer(value.opt("version"), 1) == 1 && workspaceId(value.optString("workspaceId")));
            JSONArray providers = value.optJSONArray("providers"); require(providers != null && providers.length() <= 1024);
            List<Provider> result = new ArrayList<>(); Set<String> seen = new HashSet<>(); Map<String, Integer> ownerCounts = new HashMap<>();
            for (int index = 0; index < providers.length(); ++index) {
                JSONObject item = providers.optJSONObject(index); require(keys(item, "addonId", "widgetId", "name"));
                String addon = item.optString("addonId"), widget = item.optString("widgetId");
                require(item.opt("addonId") instanceof String && item.opt("widgetId") instanceof String && addonId(addon) && widgetId(widget)
                    && text(item.opt("name"), 128, false) && !blank(item.optString("name")) && seen.add(addon + "/" + widget));
                int owned = ownerCounts.containsKey(addon) ? ownerCounts.get(addon) + 1 : 1; require(owned <= 16); ownerCounts.put(addon, owned);
                result.add(new Provider(addon, widget, item.getString("name")));
            }
            return new Catalog(value.getString("workspaceId"), result);
        } catch (Exception failure) { return null; }
    }
    static Snapshot snapshot(File root, Catalog catalog, Provider provider) {
        try {
            JSONObject value = read(root, filename(provider.addonId, provider.widgetId));
            require(keys(value, "version", "workspaceId", "addonId", "widgetId", "generation", "updatedAt", "validFrom", "validThrough", "palette", "timeline", "actions"));
            require(integer(value.opt("version"), 1) == 1 && catalog.workspaceId.equals(value.opt("workspaceId"))
                && provider.addonId.equals(value.opt("addonId")) && provider.widgetId.equals(value.opt("widgetId")));
            long generation = integer(value.opt("generation"), MAX_GENERATION), from = date(value.opt("validFrom")), through = date(value.opt("validThrough")), updated = date(value.opt("updatedAt"));
            require(from != Long.MIN_VALUE && through > from && updated >= from && updated < through);
            JSONObject palette = value.optJSONObject("palette"); require(keys(palette, "background", "text", "accent"));
            for (String key : keySet(palette)) require(palette.opt(key) instanceof String && palette.getString(key).matches("#([a-fA-F0-9]{6}|[a-fA-F0-9]{8})"));
            JSONObject actions = value.optJSONObject("actions"); require(actions != null && actions.length() <= 512 && actions.has("open"));
            for (String action : keySet(actions)) {
                require(id(action)); JSONObject data = actions.optJSONObject(action); require(data != null && data.length() <= 8);
                for (String key : keySet(data)) {
                    require(id(key)); Object scalar = data.opt(key);
                    require(scalar == JSONObject.NULL || scalar instanceof Boolean || (scalar instanceof String && text(scalar, 1024, false))
                        || (scalar instanceof Number && Double.isFinite(((Number)scalar).doubleValue())));
                }
            }
            JSONArray frames = value.optJSONArray("timeline"); require(frames != null && frames.length() >= 1 && frames.length() <= 64);
            long previous = Long.MIN_VALUE; int total = 0;
            for (int index = 0; index < frames.length(); ++index) {
                JSONObject frame = frames.optJSONObject(index);
                require(frame != null && frame.has("at") && frame.has("title") && frame.has("rows")
                    && new HashSet<>(Arrays.asList("at", "title", "rows", "heading", "timeGrid", "actionId", "clock")).containsAll(keySet(frame)));
                if (frame.has("heading")) {
                    JSONObject heading = frame.optJSONObject("heading"); require(keys(heading, "label", "value", "caption")
                        && text(heading.opt("label"), 128, false) && text(heading.opt("value"), 16, false) && text(heading.opt("caption"), 128, false));
                }
                if (frame.has("timeGrid")) require(interval(frame.optJSONObject("timeGrid")));
                if (frame.has("clock")) {
                    JSONObject clock = frame.optJSONObject("clock"); require(keys(clock, "date", "timeZone")
                        && clock.opt("date") instanceof String && clock.opt("timeZone") instanceof String);
                    String day = clock.getString("date"), zone = clock.getString("timeZone");
                    require(day.matches("[0-9]{4}-[0-9]{2}-[0-9]{2}") && java.time.LocalDate.parse(day).getYear() >= 1
                        && zone.length() <= 128 && zone.matches("[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+)*"));
                }
                if (frame.has("actionId")) require(frame.opt("actionId") instanceof String && id(frame.getString("actionId")) && actions.has(frame.getString("actionId")));
                long at = date(frame.opt("at"));
                require(at >= from && at < through && at > previous && (index != 0 || at == from) && text(frame.opt("title"), 256, false)); previous = at;
                JSONArray rows = frame.optJSONArray("rows"); require(rows != null && rows.length() <= 64 && (total += rows.length()) <= 512);
                Set<String> rowIds = new HashSet<>();
                for (int rowIndex = 0; rowIndex < rows.length(); ++rowIndex) {
                    JSONObject row = rows.optJSONObject(rowIndex); require(row != null && row.has("id") && row.has("title")
                        && new HashSet<>(Arrays.asList("id", "title", "detail", "actionId", "group", "leading", "accent", "timeRange", "allDay")).containsAll(keySet(row))
                        && id(row.optString("id")) && row.opt("id") instanceof String && rowIds.add(row.getString("id")) && text(row.opt("title"), 256, false));
                    if (row.has("detail")) require(text(row.opt("detail"), 512, true));
                    if (row.has("group")) require(text(row.opt("group"), 128, false));
                    if (row.has("leading")) require(text(row.opt("leading"), 128, true));
                    if (row.has("accent")) require(row.opt("accent") instanceof String && row.getString("accent").matches("#([a-fA-F0-9]{6}|[a-fA-F0-9]{8})"));
                    if (row.has("timeRange")) require(interval(row.optJSONObject("timeRange")));
                    if (row.has("allDay")) require(row.opt("allDay") instanceof Boolean && (!row.optBoolean("allDay") || !row.has("timeRange")));
                    if (row.has("actionId")) require(row.opt("actionId") instanceof String && id(row.getString("actionId")) && actions.has(row.getString("actionId")));
                }
            }
            return new Snapshot(value, generation, from, through);
        } catch (Exception failure) { return null; }
    }
    static boolean validActionUri(String value) {
        try {
            if (value == null || value.length() > 2048) return false; URI uri = new URI(value);
            if (!"pilon".equals(uri.getScheme()) || !"widget".equals(uri.getRawAuthority()) || uri.getRawFragment() != null || !uri.getRawPath().equals(uri.getPath())) return false;
            String[] path = uri.getPath().split("/", -1); if (path.length != 3 || !path[0].isEmpty() || !addonId(path[1]) || !widgetId(path[2])) return false;
            String query = uri.getRawQuery(); if (query == null) return false; Set<String> seen = new HashSet<>();
            for (String field : query.split("&", -1)) {
                String[] pair = field.split("=", -1); if (pair.length != 2 || !seen.add(pair[0])) return false;
                if (pair[0].equals("workspace")) { if (!workspaceId(pair[1])) return false; }
                else if (pair[0].equals("action")) { if (!id(pair[1])) return false; }
                else if (pair[0].equals("generation")) { if (!pair[1].matches("[1-9][0-9]{0,15}") || Long.parseLong(pair[1]) > MAX_GENERATION) return false; }
                else return false;
            }
            return seen.size() == 3;
        } catch (Exception failure) { return false; }
    }
    /** Check strict JSON syntax and depth before org.json's recursive parser sees input. */
    private static final class JsonBounds {
        private final String text; private int position;
        JsonBounds(String text) { this.text = text; }
        void check() { value(0); whitespace(); require(position == text.length()); }
        private void whitespace() { while (position < text.length() && " \n\r\t".indexOf(text.charAt(position)) >= 0) ++position; }
        private boolean take(char c) { whitespace(); if (position < text.length() && text.charAt(position) == c) { ++position; return true; } return false; }
        private void value(int depth) {
            whitespace(); require(position < text.length()); char c = text.charAt(position);
            if (c == '{' || c == '[') {
                require(depth < 8); ++position; boolean object = c == '{'; char close = object ? '}' : ']'; if (take(close)) return;
                do { if (object) { whitespace(); string(); require(take(':')); } value(depth + 1); if (take(close)) return; } while (take(','));
                throw new IllegalArgumentException("Invalid widget JSON");
            }
            if (c == '"') { string(); return; }
            for (String token : new String[]{"true", "false", "null"}) if (text.startsWith(token, position)) { position += token.length(); return; }
            int begin = position; if (c == '-') ++position;
            require(position < text.length()); if (text.charAt(position) == '0') ++position;
            else { int digits = position; while (position < text.length() && text.charAt(position) >= '0' && text.charAt(position) <= '9') ++position; require(position > digits); }
            if (position < text.length() && text.charAt(position) == '.') { ++position; digits(); }
            if (position < text.length() && (text.charAt(position) == 'e' || text.charAt(position) == 'E')) { ++position; if (position < text.length() && (text.charAt(position) == '+' || text.charAt(position) == '-')) ++position; digits(); }
            require(Double.isFinite(Double.parseDouble(text.substring(begin, position))));
        }
        private void digits() { int begin = position; while (position < text.length() && text.charAt(position) >= '0' && text.charAt(position) <= '9') ++position; require(position > begin); }
        private void string() {
            require(position < text.length() && text.charAt(position++) == '"');
            while (position < text.length()) {
                char c = text.charAt(position++); if (c == '"') return; require(c >= 32);
                if (c == '\\') {
                    require(position < text.length()); c = text.charAt(position++);
                    if (c == 'u') { require(position + 4 <= text.length()); for (int index = 0; index < 4; ++index) require(Character.digit(text.charAt(position++), 16) >= 0); }
                    else require("\"\\/bfnrt".indexOf(c) >= 0);
                }
            }
            throw new IllegalArgumentException("Invalid widget JSON");
        }
    }
}
