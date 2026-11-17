package org.pilon;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Instant;
import org.json.JSONArray;
import org.json.JSONObject;

/** Host checks use Android's public libcore JSON implementation, not SDK method stubs. */
public final class PilonWidgetDataChecks {
    private static final String WORKSPACE = "a".repeat(64);
    private static final String ADDON = "org.example.widget";
    private static int checks;
    private static void check(boolean condition) { if (!condition) throw new AssertionError("Widget data check " + checks); ++checks; }
    private static JSONObject snapshot() throws Exception {
        JSONObject row = new JSONObject().put("id", "row-1").put("title", "First item").put("detail", "Two\nlines").put("actionId", "open-item");
        return new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("addonId", ADDON).put("widgetId", "agenda")
            .put("generation", 3).put("updatedAt", "2026-11-09T01:00:00Z").put("validFrom", "2026-11-09T00:00:00Z").put("validThrough", "2026-11-11T00:00:00Z")
            .put("palette", new JSONObject().put("background", "#241E1A").put("text", "#FFD3C4B5").put("accent", "#FFB86F"))
            .put("actions", new JSONObject().put("open", new JSONObject()).put("open-item", new JSONObject().put("id", "opaque-id")))
            .put("timeline", new JSONArray().put(new JSONObject().put("at", "2026-11-09T00:00:00Z").put("title", "First day").put("rows", new JSONArray().put(row)))
                .put(new JSONObject().put("at", "2026-11-10T00:00:00Z").put("title", "Second day").put("rows", new JSONArray())));
    }
    private static void write(File file, String text) throws Exception { Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8)); }
    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("pilon-widget-data-").toFile();
        try {
            JSONObject provider = new JSONObject().put("addonId", ADDON).put("widgetId", "agenda").put("name", "Agenda");
            JSONObject catalog = new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", new JSONArray().put(provider));
            File catalogueFile = new File(root, "catalog.json"); write(catalogueFile, catalog.toString());
            PilonWidgetData.Catalog parsed = PilonWidgetData.catalog(root); check(parsed != null && parsed.providers.size() == 1);
            PilonWidgetData.Provider selected = parsed.providers.get(0);
            File file = new File(root, PilonWidgetData.filename(ADDON, "agenda"));
            JSONObject valid = snapshot(); write(file, valid.toString());
            PilonWidgetData.Snapshot data = PilonWidgetData.snapshot(root, parsed, selected); check(data != null && data.generation == 3);
            JSONObject grid = new JSONObject(valid.toString()); JSONObject gridFrame = grid.getJSONArray("timeline").getJSONObject(0);
            gridFrame.put("timeGrid", new JSONObject().put("startMinute", 0).put("endMinute", 1440)).put("actionId", "open-item");
            JSONObject gridRow = gridFrame.getJSONArray("rows").getJSONObject(0);
            gridRow.put("timeRange", new JSONObject().put("startMinute", 540).put("endMinute", 600)).put("allDay", false);
            write(file, grid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) != null);
            gridRow.remove("timeRange"); gridRow.put("allDay", true);
            write(file, grid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) != null);
            gridRow.put("timeRange", new JSONObject().put("startMinute", 540).put("endMinute", 600));
            write(file, grid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            for (String invalidInterval : new String[]{"{\"startMinute\":-1,\"endMinute\":60}", "{\"startMinute\":0,\"endMinute\":1441}",
                "{\"startMinute\":60,\"endMinute\":60}", "{\"startMinute\":61,\"endMinute\":60}", "{\"startMinute\":0.5,\"endMinute\":60}",
                "{\"startMinute\":false,\"endMinute\":60}", "{\"startMinute\":\"0\",\"endMinute\":60}", "{\"startMinute\":0,\"endMinute\":60,\"extra\":true}"}) {
                JSONObject invalid = new JSONObject(valid.toString()); invalid.getJSONArray("timeline").getJSONObject(0).put("timeGrid", new JSONObject(invalidInterval));
                write(file, invalid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
                invalid = new JSONObject(valid.toString()); invalid.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("timeRange", new JSONObject(invalidInterval));
                write(file, invalid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            }
            JSONObject invalidGrid = new JSONObject(valid.toString()); invalidGrid.getJSONArray("timeline").getJSONObject(0).put("actionId", "missing");
            write(file, invalidGrid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            invalidGrid = new JSONObject(valid.toString()); invalidGrid.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("allDay", "true");
            write(file, invalidGrid.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            write(file, valid.toString());
            check(data.frame(Instant.parse("2026-11-09T12:00:00Z").toEpochMilli()).getString("title").equals("First day"));
            check(data.frame(Instant.parse("2026-11-10T12:00:00Z").toEpochMilli()).getJSONArray("rows").length() == 0);
            check(data.frame(Instant.parse("2026-11-11T00:00:00Z").toEpochMilli()) == null);
            check(data.frame(Instant.parse("2026-11-08T23:59:59Z").toEpochMilli()) == null);
            check(PilonWidgetData.newlyPinnedId(2, new int[]{1}, new int[]{1, 2}) == 2);
            check(PilonWidgetData.newlyPinnedId(0, new int[]{1}, new int[]{1, 2}) == 2);
            check(PilonWidgetData.newlyPinnedId(1, new int[]{1}, new int[]{1, 2}) == 2);
            check(PilonWidgetData.newlyPinnedId(0, new int[]{1}, new int[]{1}) == 0);
            check(PilonWidgetData.newlyPinnedId(0, new int[]{1}, new int[]{1, 2, 3}) == 0);
            check(PilonWidgetData.newlyPinnedId(2, new int[]{1}, new int[]{1, 2, 3}) == 2);
            check(PilonWidgetData.newlyPinnedId(0, new int[]{1}, new int[]{1, -1}) == 0);
            for (String key : new String[]{"workspaceId", "addonId", "widgetId", "version", "generation", "updatedAt", "validFrom", "validThrough"}) {
                JSONObject bad = new JSONObject(valid.toString()).put(key, "invalid"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            }
            for (long generation : new long[]{0, -1, 9007199254740992L}) {
                JSONObject bad = new JSONObject(valid.toString()).put("generation", generation); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            }
            JSONObject bad = new JSONObject(valid.toString()).put("extra", true); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONObject("actions").put("bad", new JSONObject().put("nested", new JSONObject())); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONObject("actions").remove("open"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).put("at", "2026-11-09T00:01:00Z"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(1).put("at", "2026-11-09T00:00:00Z"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("actionId", "missing"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("title", "📝".repeat(257)); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("title", "Bad\nheading"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            for (String invalidText : new String[]{"Bad\u0080heading", "Bad\u0085heading", "Bad\u009fheading", "\\ud800", "\\udc00", "\\ud800x"}) {
                String escaped = valid.toString().replace("First item", invalidText);
                write(file, escaped); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            }
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("title", "📝".repeat(256)); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) != null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0).put("url", "https://example.invalid"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").put(new JSONObject().put("id", "row-1").put("title", "Duplicate")); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); bad.getJSONArray("timeline").getJSONObject(0).getJSONArray("rows").getJSONObject(0)
                .put("group", "Tomorrow").put("leading", "09:00\n10:00").put("accent", "#ffb86c");
            write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) != null);
            for (String field : new String[]{"label", "caption"}) {
                bad = new JSONObject(valid.toString()); JSONObject heading = new JSONObject().put("label", "Tuesday").put("value", "10").put("caption", "November 2026").put(field, "x".repeat(129));
                bad.getJSONArray("timeline").getJSONObject(0).put("heading", heading); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            }
            bad = new JSONObject(valid.toString()); bad.getJSONObject("actions").getJSONObject("open").put("value", "📝".repeat(1025)); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); for (int index = 0; index < 9; ++index) bad.getJSONObject("actions").getJSONObject("open").put("value" + index, true); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()); for (int index = 0; index < 511; ++index) bad.getJSONObject("actions").put("action" + index, new JSONObject()); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()).put("updatedAt", "2026-11-11T00:00:00Z"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()).put("validThrough", "2026-11-11T00:00:00.123456789Z"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            bad = new JSONObject(valid.toString()).put("validFrom", "0000-01-01T00:00:00Z"); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            JSONArray maximumFrames = new JSONArray();
            for (int frame = 0; frame < 64; ++frame) {
                JSONArray items = new JSONArray(); for (int rowIndex = 0; rowIndex < 8; ++rowIndex) items.put(new JSONObject().put("id", "row" + rowIndex).put("title", "Item"));
                maximumFrames.put(new JSONObject().put("at", Instant.parse("2026-11-09T00:00:00Z").plusMillis(frame).toString()).put("title", "Frame").put("rows", items));
            }
            bad = new JSONObject(valid.toString()).put("timeline", maximumFrames); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) != null);
            maximumFrames.getJSONObject(0).getJSONArray("rows").put(new JSONObject().put("id", "extra").put("title", "Extra")); write(file, bad.toString()); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            for (String malformed : new String[]{"{unquoted:true}", "{\"key\":01}", "{\"key\":1e999}", "{\"key\":true,}", "{\"key\":/*comment*/true}"}) {
                boolean rejected = false; try { PilonWidgetData.parse(malformed); } catch (Exception failure) { rejected = true; } check(rejected);
            }
            write(file, valid.toString() + " trailing"); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            write(file, "{" + "\"nested\":".repeat(1) + "[".repeat(20000) + "0" + "]".repeat(20000) + "}"); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            write(file, " ".repeat(262145)); check(PilonWidgetData.snapshot(root, parsed, selected) == null);
            write(file, valid.toString()); File outside = File.createTempFile("pilon-widget-outside-", ".json");
            try { Files.delete(file.toPath()); Files.createSymbolicLink(file.toPath(), outside.toPath()); check(PilonWidgetData.snapshot(root, parsed, selected) == null); }
            finally { Files.deleteIfExists(file.toPath()); Files.deleteIfExists(outside.toPath()); }
            catalog.getJSONArray("providers").put(provider); write(catalogueFile, catalog.toString()); check(PilonWidgetData.catalog(root) == null);
            for (String whitespace : new String[]{"\u00a0", "\u0085", "\u2007"}) {
                JSONObject badCatalog = new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", new JSONArray().put(new JSONObject(provider.toString()).put("name", whitespace)));
                write(catalogueFile, badCatalog.toString()); check(PilonWidgetData.catalog(root) == null);
            }
            JSONArray excessive = new JSONArray(); for (int index = 0; index < 17; ++index) excessive.put(new JSONObject(provider.toString()).put("widgetId", "widget-" + index));
            write(catalogueFile, new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", excessive).toString()); check(PilonWidgetData.catalog(root) == null);
            check(!PilonWidgetData.validActionUri("pilon://widget/../agenda?workspace=" + WORKSPACE + "&action=open&generation=1"));
            check(PilonWidgetData.validActionUri("pilon://widget/" + ADDON + "/agenda?workspace=" + WORKSPACE + "&action=open&generation=3"));
            for (String invalid : new String[]{"pilon://other/", "pilon://user@widget/", "pilon://widget:80/"}) check(!PilonWidgetData.validActionUri(invalid + ADDON + "/agenda?workspace=" + WORKSPACE + "&action=open&generation=3"));
            for (String suffix : new String[]{"&extra=1", "&action=open", "#fragment"}) check(!PilonWidgetData.validActionUri("pilon://widget/" + ADDON + "/agenda?workspace=" + WORKSPACE + "&action=open&generation=3" + suffix));
            if (args.length == 1) {
                JSONObject vectors = new JSONObject(Files.readString(new File(args[0]).toPath()));
                JSONObject fixtureProvider = new JSONObject().put("addonId", "org.example.fixture").put("widgetId", "list").put("name", "Fixture");
                write(catalogueFile, new JSONObject().put("version", 1).put("workspaceId", WORKSPACE).put("providers", new JSONArray().put(fixtureProvider)).toString());
                PilonWidgetData.Catalog fixtureCatalog = PilonWidgetData.catalog(root); check(fixtureCatalog != null);
                File fixtureFile = new File(root, PilonWidgetData.filename("org.example.fixture", "list")); JSONArray cases = vectors.getJSONArray("cases");
                for (int index = 0; index < cases.length(); ++index) {
                    JSONObject test = cases.getJSONObject(index); write(fixtureFile, test.getJSONObject("snapshot").toString());
                    boolean accepted = PilonWidgetData.snapshot(root, fixtureCatalog, fixtureCatalog.providers.get(0)) != null;
                    if (accepted != test.getBoolean("schemaValid")) throw new AssertionError("Shared widget vector disagrees: " + test.getString("name"));
                    ++checks;
                }
                System.out.println("Shared widget contract vectors passed: " + cases.length());
            }
            System.out.println("Widget data checks passed: " + checks);
        } finally {
            for (File child : root.listFiles()) Files.deleteIfExists(child.toPath()); Files.deleteIfExists(root.toPath());
        }
    }
}
