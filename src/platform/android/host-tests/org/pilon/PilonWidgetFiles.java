package org.pilon;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.SeekableByteChannel;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardOpenOption;
import java.util.Set;

/** Host JSON fixtures only; excluded from both Android application and androidTest source sets. */
final class PilonWidgetFiles {
    private PilonWidgetFiles() {}
    static byte[] bytes(File root, String filename, int maximum) throws Exception {
        File file = new File(root, filename);
        if (!(filename.equals("catalog.json") || filename.matches("[a-f0-9]{64}\\.json"))
            || !Files.isDirectory(root.toPath(), LinkOption.NOFOLLOW_LINKS)
            || !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)
            || ((Number)Files.getAttribute(file.toPath(), "unix:nlink", LinkOption.NOFOLLOW_LINKS)).longValue() != 1)
            throw new IOException("Invalid widget fixture file");
        try (SeekableByteChannel input = Files.newByteChannel(file.toPath(), Set.of(StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS))) {
            if (input.size() > maximum) throw new IOException("Widget fixture exceeds its limit");
            ByteArrayOutputStream result = new ByteArrayOutputStream(); ByteBuffer buffer = ByteBuffer.allocate(4096);
            while (input.read(buffer) > 0) {
                if (result.size() + buffer.position() > maximum) throw new IOException("Widget fixture exceeds its limit");
                result.write(buffer.array(), 0, buffer.position()); buffer.clear();
            }
            return result.toByteArray();
        }
    }
}
