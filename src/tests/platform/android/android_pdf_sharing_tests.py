#!/usr/bin/env python3
"""Exercise the production share-file cache with real temporary files."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def verify(source, jdk):
    harness = r'''
package org.pilon;
import static org.pilon.PilonShareCache.retainFile;
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.attribute.FileTime;
import java.util.Arrays;
public class SharingCheck {
    static int checks;
    static void require(boolean value) { if (!value) throw new AssertionError(); ++checks; }
    public static void main(String[] args) throws Exception {
        Path root=Path.of(args[0]); Path source=Files.createDirectory(root.resolve("source"));
        Path cache=Files.createDirectory(root.resolve("cache"));
        byte[] first="%PDF-1.7\nfirst export\n%%EOF".getBytes(StandardCharsets.US_ASCII);
        Path original=source.resolve("My note.pdf"); Files.write(original,first);
        File retained=retainFile(original.toFile(),cache.toFile(),true);
        require(retained.toPath().startsWith(cache.resolve("pdf-shares")));
        require(retained.getName().equals("My note.pdf"));
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        Files.delete(original); Files.delete(source);
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        Files.createDirectory(source); Files.write(original,"%PDF-1.7\nsecond export\n%%EOF".getBytes(StandardCharsets.US_ASCII));
        File second=retainFile(original.toFile(),cache.toFile(),true);
        require(!second.equals(retained));
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        require(!Arrays.equals(Files.readAllBytes(second.toPath()),first));
        Files.setLastModifiedTime(retained.toPath(),FileTime.fromMillis(System.currentTimeMillis()-2*86400000L));
        retainFile(original.toFile(),cache.toFile(),true);
        require(!Files.exists(retained.toPath()));
        require(!Files.exists(retained.toPath().getParent()));
        require(Files.exists(second.toPath()));
        for (byte[] invalid:new byte[][] {new byte[0],"%PD".getBytes(),"hello-not-a-pdf".getBytes()}) {
            Files.write(original,invalid); boolean failed=false;
            try { retainFile(original.toFile(),cache.toFile(),true); } catch(IOException error) { failed=true; }
            require(failed);
        }
        Files.delete(original); boolean failed=false;
        try { retainFile(original.toFile(),cache.toFile(),true); } catch(IOException error) { failed=true; }
        require(failed);
        Files.write(original,first); Path blocked=root.resolve("blocked"); Files.write(blocked,new byte[]{1});failed=false;
        try { retainFile(original.toFile(),blocked.toFile(),true); } catch(IOException error) { failed=true; }
        require(failed); require(Arrays.equals(Files.readAllBytes(original),first));
        Path generic=source.resolve("shared file.bin");
        byte[] payload={0,1,2,42}; Files.write(generic,payload);
        File attachment=retainFile(generic.toFile(),cache.toFile(),false);
        require(attachment.getName().equals("shared file.bin"));
        Files.delete(generic);
        require(Arrays.equals(Files.readAllBytes(attachment.toPath()),payload));
        failed=false;
        try { retainFile(source.toFile(),cache.toFile(),false); } catch(IOException error) { failed=true; }
        require(failed);
        Path bounded=Files.createDirectory(root.resolve("bounded"));
        Files.write(generic,payload);
        File protectedShare=retainFile(generic.toFile(),bounded.toFile(),false,7,2);
        failed=false;
        try { retainFile(generic.toFile(),bounded.toFile(),false,7,2); } catch(IOException error) { failed=true; }
        require(failed);
        require(Arrays.equals(Files.readAllBytes(protectedShare.toPath()),payload));
        try (DirectoryStream<Path> folders=Files.newDirectoryStream(bounded.resolve("pdf-shares"))) {
            require(folders.iterator().next().equals(protectedShare.toPath().getParent()));
        }
        Files.write(generic,new byte[0]);
        failed=false;
        try { retainFile(generic.toFile(),bounded.toFile(),false,8,1); } catch(IOException error) { failed=true; }
        require(failed);
        require(Arrays.equals(Files.readAllBytes(protectedShare.toPath()),payload));
        System.out.println("Share retention checks passed: "+checks);
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="pilon-sharing-") as temporary:
        folder = Path(temporary)
        java = folder / "SharingCheck.java"
        java.write_text(harness)
        production = source.with_name("PilonShareCache.java")
        subprocess.run([str(jdk / "bin/javac"), "--release", "17", "-d", str(folder), str(production), str(java)], check=True)
        data = folder / "data"
        data.mkdir()
        subprocess.run([str(jdk / "bin/java"), "-cp", str(folder), "org.pilon.SharingCheck", str(data)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--jdk", type=Path, required=True)
    args = parser.parse_args()
    verify(args.source, args.jdk)
