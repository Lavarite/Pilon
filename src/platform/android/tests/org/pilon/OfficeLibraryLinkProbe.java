package org.pilon;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Real filesystem regression; no Android activity, package install or converter is needed. */
public final class OfficeLibraryLinkProbe {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void run(File folder) throws Exception {
        Path root = folder.toPath();
        Files.createDirectories(root);
        Path old = Files.createDirectory(root.resolve("old-apk-native"));
        Path current = Files.createDirectory(root.resolve("current-apk-native"));
        Path link = root.resolve("lib");
        PilonOffice.NativeLibraryLink.refresh(link.toFile(), old.toFile());
        require(Files.readSymbolicLink(link).equals(old.toRealPath()), "Initial native alias is incorrect");
        Files.delete(old);
        PilonOffice.NativeLibraryLink.refresh(link.toFile(), current.toFile());
        require(Files.readSymbolicLink(link).equals(current.toRealPath()), "Stale native alias was not retargeted after update");
        PilonOffice.NativeLibraryLink.refresh(link.toFile(), current.toFile());
        require(Files.readSymbolicLink(link).equals(current.toRealPath()), "Repeated alias refresh is not stable");
        byte[] sentinel = "Unrelated data must survive".getBytes(StandardCharsets.UTF_8);
        Path unrelated = Files.createDirectory(root.resolve("unrelated"));
        Files.write(unrelated.resolve("keep"), sentinel);
        Files.delete(link); Files.createSymbolicLink(link, unrelated);
        PilonOffice.NativeLibraryLink.refresh(link.toFile(), current.toFile());
        require(Arrays.equals(sentinel, Files.readAllBytes(unrelated.resolve("keep"))), "Alias refresh changed an unrelated target");
        Files.delete(link); Files.write(link, sentinel);
        boolean rejected = false;
        try { PilonOffice.NativeLibraryLink.refresh(link.toFile(), current.toFile()); }
        catch (Exception expected) { rejected = true; }
        require(rejected && Arrays.equals(sentinel, Files.readAllBytes(link)), "Regular-file collision was overwritten");
        Files.delete(link); Files.createDirectory(link); Files.write(link.resolve("keep"), sentinel);
        rejected = false;
        try { PilonOffice.NativeLibraryLink.refresh(link.toFile(), current.toFile()); }
        catch (Exception expected) { rejected = true; }
        require(rejected && Arrays.equals(sentinel, Files.readAllBytes(link.resolve("keep"))), "Directory collision was overwritten");
        try (java.util.stream.Stream<Path> entries = Files.list(root)) {
            require(entries.noneMatch(path -> path.getFileName().toString().startsWith(".lib-")), "Temporary alias leaked");
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("pilon-office-link-");
        try { run(root.toFile()); System.out.println("Office native-library alias upgrade checks passed"); }
        finally {
            try (java.util.stream.Stream<Path> entries = Files.walk(root)) {
                entries.sorted(java.util.Comparator.reverseOrder()).forEach(path -> path.toFile().delete());
            }
        }
    }
}
