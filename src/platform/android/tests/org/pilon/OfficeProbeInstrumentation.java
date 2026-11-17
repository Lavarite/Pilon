package org.pilon;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Instrumentation;
import android.content.Context;
import android.graphics.pdf.PdfRenderer;
import android.graphics.pdf.content.PdfPageTextContent;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** Tests the actual bound Office service under Pilon's installed UID. */
public final class OfficeProbeInstrumentation extends Instrumentation {
    private int checks;
    private File directory;
    private final Set<Long> pending = new HashSet<>();
    private final JSONArray facts = new JSONArray();

    private void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
        ++checks;
    }

    private long start(String name, File output) {
        long id = PilonOffice.start(getTargetContext(), new File(directory, name).getPath(), output.getPath());
        pending.add(id);
        return id;
    }

    private String await(long id) throws Exception {
        long limit = System.nanoTime() + 60_000_000_000L;
        while (System.nanoTime() < limit) {
            String result = PilonOffice.takeResult(id);
            if (result != null) { pending.remove(id); return result; }
            Thread.sleep(5);
        }
        throw new AssertionError("Office service did not finish");
    }

    private JSONObject inspect(File file) throws Exception {
        StringBuilder text = new StringBuilder();
        try (ParcelFileDescriptor fd = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
             PdfRenderer pdf = new PdfRenderer(fd)) {
            for (int i = 0; i < pdf.getPageCount(); ++i) {
                try (PdfRenderer.Page page = pdf.openPage(i)) {
                    for (PdfPageTextContent content : page.getTextContents()) text.append(content.getText()).append(' ');
                }
            }
            return new JSONObject().put("pages", pdf.getPageCount()).put("text", text.toString().replaceAll("\\s+", " "));
        }
    }

    private JSONArray extract() throws Exception {
        try (ZipInputStream archive = new ZipInputStream(getContext().getAssets().open("office-fixtures.zip"))) {
            for (ZipEntry entry; (entry = archive.getNextEntry()) != null;) {
                require(!entry.getName().contains("/") && !entry.getName().contains(".."), "Unsafe fixture path");
                try (FileOutputStream output = new FileOutputStream(new File(directory, entry.getName()))) {
                    byte[] buffer = new byte[65536];
                    for (int size; (size = archive.read(buffer)) != -1;) output.write(buffer, 0, size);
                }
            }
        }
        return new JSONArray(new String(Files.readAllBytes(new File(directory, "manifest.json").toPath()), StandardCharsets.UTF_8));
    }

    private void slowDocument() throws Exception {
        try (ZipInputStream input = new ZipInputStream(Files.newInputStream(new File(directory, "document.docx").toPath()));
             ZipOutputStream output = new ZipOutputStream(Files.newOutputStream(new File(directory, "slow.docx").toPath()))) {
            for (ZipEntry entry; (entry = input.getNextEntry()) != null;) {
                output.putNextEntry(new ZipEntry(entry.getName()));
                if (entry.getName().equals("word/document.xml")) {
                    StringBuilder body = new StringBuilder("<?xml version=\"1.0\"?><w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>");
                    for (int i = 0; i < 5000; ++i) body.append("<w:p><w:r><w:t>Office cancellation fixture ").append(i).append("</w:t></w:r></w:p>");
                    body.append("<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/></w:sectPr></w:body></w:document>");
                    output.write(body.toString().getBytes(StandardCharsets.UTF_8));
                } else {
                    byte[] buffer = new byte[65536];
                    for (int size; (size = input.read(buffer)) != -1;) output.write(buffer, 0, size);
                }
                output.closeEntry();
            }
        }
    }

    private void awaitPartial(long id, File output) throws Exception {
        File partial = new File(directory, "." + output.getName() + "." + id + ".part.pdf");
        long limit = System.nanoTime() + 60_000_000_000L;
        while (!partial.isFile() && System.nanoTime() < limit) {
            String result = PilonOffice.takeResult(id);
            if (result != null) { pending.remove(id); throw new AssertionError("Conversion finished before its partial output was observed: " + result); }
            Thread.sleep(1);
        }
        require(partial.isFile(), "Native conversion did not create partial output");
    }

    private int officePid() {
        ActivityManager manager = getTargetContext().getSystemService(ActivityManager.class);
        for (ActivityManager.RunningAppProcessInfo process : manager.getRunningAppProcesses())
            if (process.uid == Process.myUid() && process.processName.equals(getTargetContext().getPackageName() + ":office")) return process.pid;
        return 0;
    }

    private JSONArray exits(long since) throws Exception {
        JSONArray result = new JSONArray();
        ActivityManager manager = getTargetContext().getSystemService(ActivityManager.class);
        for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(getTargetContext().getPackageName(), 0, 10))
            if (exit.getProcessName().endsWith(":office") && exit.getTimestamp() >= since)
                result.put(new JSONObject().put("reason", exit.getReason()).put("status", exit.getStatus()).put("description", exit.getDescription()));
        return result;
    }

    private void checks() throws Exception {
        Context context = getTargetContext();
        require(context.getApplicationInfo().uid == Process.myUid(), "Probe is not running under app UID");
        directory = new File(context.getCacheDir(), "pilon-office-probe-" + UUID.randomUUID());
        require(directory.mkdir(), "Cannot create isolated fixture directory");
        OfficeLibraryLinkProbe.run(new File(directory, "library-alias-upgrade"));
        require(true, "Native library alias upgrade and collision checks passed");
        JSONArray fixtures = extract();
        require(fixtures.length() == 15, "Office format matrix changed");
        ArrayList<String> failures = new ArrayList<>();
        for (int i = 0; i < fixtures.length(); ++i) {
            JSONObject fixture = fixtures.getJSONObject(i);
            String name = fixture.getString("filename");
            byte[] original = Files.readAllBytes(new File(directory, name).toPath());
            File output = new File(directory, name + ".pdf");
            long started = System.currentTimeMillis();
            String error = await(start(name, output));
            JSONObject fact = new JSONObject().put("filename", name).put("error", error).put("milliseconds", System.currentTimeMillis() - started);
            facts.put(fact);
            if (!error.isEmpty()) { fact.put("exits", exits(started)); failures.add(name + ": " + error); continue; }
            JSONObject document = inspect(output); fact.put("document", document);
            require(document.getInt("pages") == fixture.getInt("pages"), name + " page count changed");
            JSONArray expected = fixture.getJSONArray("contains");
            for (int j = 0; j < expected.length(); ++j)
                require(document.getString("text").contains(expected.getString(j)), name + " lost expected text");
            JSONArray excluded = fixture.optJSONArray("notContains");
            if (excluded != null) for (int j = 0; j < excluded.length(); ++j)
                require(!document.getString("text").contains(excluded.getString(j)), name + " did not split delimited cells");
            require(Arrays.equals(original, Files.readAllBytes(new File(directory, name).toPath())), name + " source changed");
        }
        byte[] sentinel = "Existing output must remain unchanged".getBytes(StandardCharsets.UTF_8);
        File output = new File(directory, "preserved.pdf");
        Files.write(output.toPath(), sentinel);
        long queued = start("document.docx", output); PilonOffice.cancel(queued);
        require(await(queued).toLowerCase(java.util.Locale.ROOT).contains("cancel"), "Queued cancellation was not reported");
        require(Arrays.equals(sentinel, Files.readAllBytes(output.toPath())), "Queued cancel replaced existing output");
        slowDocument();
        long active = start("slow.docx", output); awaitPartial(active, output); PilonOffice.cancel(active);
        require(await(active).toLowerCase(java.util.Locale.ROOT).contains("cancel"), "Native conversion cancellation was not reported");
        require(Arrays.equals(sentinel, Files.readAllBytes(output.toPath())), "Native cancellation replaced existing output");
        File recovery = new File(directory, "recovery.pdf");
        require(await(start("document.docx", recovery)).isEmpty() && inspect(recovery).getInt("pages") == 2, "Converter did not recover after cancellation");
        long crashed = start("slow.docx", output); awaitPartial(crashed, output);
        int process = officePid(); require(process > 0 && process != Process.myPid(), "Office is not isolated from the app process");
        Process.killProcess(process);
        require(!await(crashed).isEmpty(), "Office process death was not reported");
        require(Arrays.equals(sentinel, Files.readAllBytes(output.toPath())), "Office process death replaced existing output");
        require(await(start("document.docx", recovery)).isEmpty(), "Converter did not restart after process death");
        try (ZipOutputStream invalid = new ZipOutputStream(Files.newOutputStream(new File(directory, "invalid.docx").toPath()))) {
            String[][] entries = {
                {"[Content_Types].xml", "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/></Types>"},
                {"_rels/.rels", "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/></Relationships>"},
                {"word/document.xml", "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body><w:p>"}
            };
            for (String[] entry : entries) {
                invalid.putNextEntry(new ZipEntry(entry[0])); invalid.write(entry[1].getBytes(StandardCharsets.UTF_8)); invalid.closeEntry();
            }
        }
        require(!await(start("invalid.docx", output)).isEmpty(), "Invalid Office input was accepted");
        require(Arrays.equals(sentinel, Files.readAllBytes(output.toPath())), "Conversion failure replaced existing output");
        require(directory.listFiles((folder, name) -> name.endsWith(".part.pdf")).length == 0, "Partial Office output leaked");
        require(failures.isEmpty(), "Office formats failed: " + String.join(", ", failures));
    }

    private void remove(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) remove(child);
        file.delete();
    }

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        int status = Activity.RESULT_CANCELED;
        try {
            checks(); result.putInt("checks", checks); result.putString("stream", "Pilon Office checks passed: " + checks + "\n");
            status = Activity.RESULT_OK;
        } catch (Throwable failure) {
            result.putString("stream", "Pilon Office checks failed: " + failure + "\n");
        } finally {
            for (long id : new ArrayList<>(pending)) {
                PilonOffice.cancel(id);
                try { await(id); } catch (Throwable ignored) { PilonOffice.forget(id); }
            }
            if (directory != null) remove(directory);
        }
        result.putString("facts", facts.toString());
        finish(status, result);
    }
}
