package org.pilon;

import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** Run in a self-targeted instrumentation APK, using an isolated private temporary directory. */
public final class PilonWidgetFilesDeviceChecks {
    private static int checks;
    static String step = "not-started";
    static String skipped = "";
    private static void check(boolean value) { if (!value) throw new AssertionError("Widget native IO check " + checks); ++checks; }
    private static boolean rejected(File directory) {
        try { PilonWidgetFiles.bytes(directory, "catalog.json", 262144); return false; }
        catch (Exception expected) { return true; }
    }
    private static void unsupportedSetup(String fixture, ErrnoException failure) throws ErrnoException {
        if (failure.errno != OsConstants.EACCES && failure.errno != OsConstants.EPERM
            && failure.errno != OsConstants.EOPNOTSUPP && failure.errno != OsConstants.ENOSYS) throw failure;
        skipped += (skipped.isEmpty() ? "" : ", ") + fixture + "(errno=" + failure.errno + ")";
    }
    public static void main(String[] args) throws Exception {
        if (args.length != 1) throw new IllegalArgumentException("Pass an existing app-private test parent directory");
        System.out.println("Widget native IO checks passed: " + run(args[0]));
    }
    static int run(String parent) throws Exception {
        checks = 0;
        skipped = "";
        step = "create-private-temp-directory";
        File directory = Files.createTempDirectory(new File(parent).toPath(), "pilon-widget-io-").toFile();
        File file = new File(directory, "catalog.json"), other = new File(directory, "other.json");
        try {
            byte[] content = "{\"fixture\":true}".getBytes(StandardCharsets.UTF_8);
            step = "write-normal-fixture"; Files.write(file.toPath(), content);
            step = "read-normal-file"; check(Arrays.equals(PilonWidgetFiles.bytes(directory, "catalog.json", 262144), content));
            step = "empty-file"; Files.write(file.toPath(), new byte[0]); check(PilonWidgetFiles.bytes(directory, "catalog.json", 262144).length == 0);
            step = "oversize-file"; Files.write(file.toPath(), new byte[262145]); check(rejected(directory)); Files.delete(file.toPath());
            step = "create-symlink-fixture"; Files.write(other.toPath(), content); Os.symlink(other.getPath(), file.getPath());
            step = "reject-symlink"; check(rejected(directory)); Files.delete(file.toPath());
            step = "create-hardlink-fixture"; boolean hardlink = false;
            try { Os.link(other.getPath(), file.getPath()); hardlink = true; }
            catch (ErrnoException failure) { unsupportedSetup("hardlink", failure); }
            if (hardlink) { step = "reject-hardlink"; check(rejected(directory)); Files.delete(file.toPath()); }
            Files.delete(other.toPath());
            step = "create-fifo-fixture"; boolean fifo = false;
            try { Os.mkfifo(file.getPath(), 0600); fifo = true; }
            catch (ErrnoException failure) { unsupportedSetup("fifo", failure); }
            if (fifo) {
                step = "reject-fifo-without-blocking"; long begin = System.nanoTime(); check(rejected(directory));
                check(System.nanoTime() - begin < 1000000000L); Files.delete(file.toPath());
            }
            step = "missing-file"; check(rejected(directory));
            step = "directory-as-file"; Files.createDirectory(file.toPath()); check(rejected(directory)); Files.delete(file.toPath());
            Files.write(file.toPath(), content);
            step = "create-symlink-directory-fixture"; File link = new File(directory, "linked-directory"); Os.symlink(directory.getPath(), link.getPath());
            step = "reject-symlink-directory";
            try { check(rejected(link)); } finally { Files.delete(link.toPath()); }
            return checks;
        } finally {
            for (File child : directory.listFiles()) Files.deleteIfExists(child.toPath());
            Files.deleteIfExists(directory.toPath());
        }
    }
}
