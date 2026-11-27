package org.pilon;

import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;

final class PilonShareCache {
    private static final long RETENTION = 24 * 60 * 60 * 1000L;
    private static final long MAX_BYTES = 1024L * 1024 * 1024;
    private static final int MAX_SHARES = 128;

    private PilonShareCache() {}

    static File retainFile(File source, File cache, boolean requirePdf) throws IOException {
        return retainFile(source, cache, requirePdf, MAX_BYTES, MAX_SHARES);
    }

    static synchronized File retainFile(File source, File cache, boolean requirePdf, long maxBytes, int maxShares) throws IOException {
        if (!source.isFile()) throw new IOException("Missing file");
        Path root = cache.toPath().resolve("pdf-shares");
        Files.createDirectories(root);
        if (!Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) throw new IOException("Invalid share cache");

        long remaining = maxBytes;
        int shares = 0;
        long oldest = System.currentTimeMillis() - RETENTION;
        // Receivers may open a shared file after the chooser returns.
        try (DirectoryStream<Path> folders = Files.newDirectoryStream(root, "share-*")) {
            for (Path folder : folders) {
                if (!Files.isDirectory(folder, LinkOption.NOFOLLOW_LINKS)) continue;
                boolean occupied = false;
                try (DirectoryStream<Path> entries = Files.newDirectoryStream(folder)) {
                    for (Path entry : entries) {
                        if (!Files.isRegularFile(entry, LinkOption.NOFOLLOW_LINKS)) { occupied = true; continue; }
                        if (Files.getLastModifiedTime(entry, LinkOption.NOFOLLOW_LINKS).toMillis() <= oldest) {
                            Files.deleteIfExists(entry);
                        } else {
                            occupied = true;
                            remaining = Math.max(0, remaining - Files.size(entry));
                        }
                    }
                }
                if (occupied) ++shares;
                else Files.deleteIfExists(folder);
            }
        }
        if (shares >= maxShares || Files.size(source.toPath()) > remaining) throw new IOException("Share cache is full");

        try (DataInputStream input = new DataInputStream(Files.newInputStream(source.toPath()))) {
            byte[] header = new byte[requirePdf ? 5 : 0];
            input.readFully(header);
            if (requirePdf && !new String(header, StandardCharsets.US_ASCII).equals("%PDF-")) throw new IOException("Invalid PDF");
            if (header.length > remaining) throw new IOException("Share cache is full");
            Path folder = Files.createTempDirectory(root, "share-");
            Path retained = folder.resolve(source.getName());
            try (OutputStream output = Files.newOutputStream(retained)) {
                output.write(header);
                remaining -= header.length;
                byte[] buffer = new byte[8192];
                for (int count; (count = input.read(buffer)) != -1;) {
                    if (count > remaining) throw new IOException("Share cache is full");
                    output.write(buffer, 0, count);
                    remaining -= count;
                }
            } catch (IOException error) {
                Files.deleteIfExists(retained);
                Files.deleteIfExists(folder);
                throw error;
            }
            return retained.toFile();
        }
    }
}
