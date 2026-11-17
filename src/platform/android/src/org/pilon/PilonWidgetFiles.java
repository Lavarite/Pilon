package org.pilon;

import android.system.Os;
import android.system.OsConstants;
import android.system.StructStat;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;

/** Opens derived cache files without following a final link or blocking on special files. */
final class PilonWidgetFiles {
    private PilonWidgetFiles() {}
    static byte[] bytes(File root, String filename, int maximum) throws Exception {
        if (maximum < 0 || !(filename.equals("catalog.json") || filename.matches("[a-f0-9]{64}\\.json"))
            || !OsConstants.S_ISDIR(Os.lstat(root.getPath()).st_mode)) throw new IOException("Invalid widget directory");
        File file = new File(root, filename);
        FileDescriptor descriptor = Os.open(file.getPath(), OsConstants.O_RDONLY | OsConstants.O_NOFOLLOW
            | OsConstants.O_NONBLOCK | OsConstants.O_CLOEXEC, 0);
        try {
            StructStat info = Os.fstat(descriptor);
            if (!OsConstants.S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 || info.st_size > maximum)
                throw new IOException("Invalid widget file");
            ByteArrayOutputStream result = new ByteArrayOutputStream((int)info.st_size);
            byte[] buffer = new byte[4096]; int count;
            while ((count = Os.read(descriptor, buffer, 0, buffer.length)) > 0) {
                if (result.size() + count > maximum) throw new IOException("Widget file exceeds its limit");
                result.write(buffer, 0, count);
            }
            return result.toByteArray();
        } finally { Os.close(descriptor); }
    }
}
