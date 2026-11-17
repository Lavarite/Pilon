#!/usr/bin/env python3
"""Exercise the production share-file retention method with real temporary files."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def verify(source, jdk):
    text = source.read_text()
    start = text.index("static File retainPdfFile(")
    end = text.index("{", start)
    depth = 1
    while depth:
        end += 1
        depth += (text[end] == "{") - (text[end] == "}")
    method = text[start:end + 1]
    harness = r'''
import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
public class SharingCheck {
    /* SOURCE_METHOD */
    static int checks;
    static void require(boolean value) { if (!value) throw new AssertionError(); ++checks; }
    public static void main(String[] args) throws Exception {
        Path root=Path.of(args[0]); Path source=Files.createDirectory(root.resolve("source"));
        Path cache=Files.createDirectory(root.resolve("cache"));
        byte[] first="%PDF-1.7\nfirst export\n%%EOF".getBytes(StandardCharsets.US_ASCII);
        Path original=source.resolve("My note.pdf"); Files.write(original,first);
        File retained=retainPdfFile(original.toFile(),cache.toFile());
        require(retained.toPath().startsWith(cache.resolve("pdf-shares")));
        require(retained.getName().equals("My note.pdf"));
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        Files.delete(original); Files.delete(source);
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        Files.createDirectory(source); Files.write(original,"%PDF-1.7\nsecond export\n%%EOF".getBytes(StandardCharsets.US_ASCII));
        File second=retainPdfFile(original.toFile(),cache.toFile());
        require(!second.equals(retained));
        require(Arrays.equals(Files.readAllBytes(retained.toPath()),first));
        require(!Arrays.equals(Files.readAllBytes(second.toPath()),first));
        for (byte[] invalid:new byte[][] {new byte[0],"%PD".getBytes(),"hello-not-a-pdf".getBytes()}) {
            Files.write(original,invalid); boolean failed=false;
            try { retainPdfFile(original.toFile(),cache.toFile()); } catch(IOException error) { failed=true; }
            require(failed);
        }
        Files.delete(original); boolean failed=false;
        try { retainPdfFile(original.toFile(),cache.toFile()); } catch(IOException error) { failed=true; }
        require(failed);
        Files.write(original,first); Path blocked=root.resolve("blocked"); Files.write(blocked,new byte[]{1});failed=false;
        try { retainPdfFile(original.toFile(),blocked.toFile()); } catch(IOException error) { failed=true; }
        require(failed); require(Arrays.equals(Files.readAllBytes(original),first));
        System.out.println("PDF retention checks passed: "+checks);
    }
}
'''.replace("/* SOURCE_METHOD */", method)
    with tempfile.TemporaryDirectory(prefix="pilon-sharing-") as temporary:
        folder = Path(temporary)
        java = folder / "SharingCheck.java"
        java.write_text(harness)
        subprocess.run([str(jdk / "bin/javac"), "--release", "17", "-d", str(folder), str(java)], check=True)
        data = folder / "data"
        data.mkdir()
        subprocess.run([str(jdk / "bin/java"), "-cp", str(folder), "SharingCheck", str(data)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--jdk", type=Path, required=True)
    args = parser.parse_args()
    verify(args.source, args.jdk)
