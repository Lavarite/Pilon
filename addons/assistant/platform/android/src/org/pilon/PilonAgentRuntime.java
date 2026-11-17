package org.pilon;

import android.content.Context;
import android.system.Os;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.json.JSONObject;

/** Native code stays in the APK; project sources and shared JS packages stay in the workspace. */
public final class PilonAgentRuntime {
    private PilonAgentRuntime() {}

    private static void copy(InputStream source, File target) throws Exception {
        Files.createDirectories(target.getParentFile().toPath());
        File temporary = new File(target.getParentFile(), "." + target.getName() + ".pilon-" + UUID.randomUUID());
        try {
            try (OutputStream destination = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[65536];
                int count;
                while ((count = source.read(buffer)) != -1) destination.write(buffer, 0, count);
            }
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } finally { temporary.delete(); }
    }

    private static String read(Context context, String path) throws Exception {
        try (InputStream input = context.getAssets().open(path);
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192]; int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static void link(File source, File target) throws Exception {
        Files.createDirectories(target.getParentFile().toPath());
        if (Files.isSymbolicLink(target.toPath())) {
            if (Os.readlink(target.getPath()).equals(source.getPath())) return;
        } else if (target.exists()) {
            throw new IOException("A project runtime link was replaced by a regular file.");
        }
        if (!source.isFile()) throw new IOException("A packaged project runtime is missing.");
        File temporary = new File(target.getParentFile(), "." + target.getName() + ".pilon-" + UUID.randomUUID());
        try {
            Os.symlink(source.getPath(), temporary.getPath());
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } finally { temporary.delete(); }
    }

    public static synchronized String prepare(Context context, String toolsDirectory) {
        try {
            JSONObject metadata = new JSONObject(read(context, "agent-runtime/runtime.json"));
            String version = metadata.getString("version");
            if (!version.matches("[a-f0-9]{64}")) throw new IOException("Invalid runtime version.");
            File privateRoot = new File(context.getNoBackupFilesDir(), "agent-runtime").getCanonicalFile();
            File bundled = new File(privateRoot, version);
            File node = new File(toolsDirectory, "node-android").getCanonicalFile();
            File privateReady = new File(bundled, ".ready");
            File nodeReady = new File(node, ".pilon-runtime-ready");
            boolean privateInstalled = privateReady.isFile();
            boolean nodeInstalled = nodeReady.isFile() && nodeReady.length() == 64 && new String(Files.readAllBytes(nodeReady.toPath()), StandardCharsets.UTF_8).equals(version);
            if (!privateInstalled || !nodeInstalled) {
                try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("agent-runtime/runtime.zip"))) {
                    ZipEntry entry;
                    while ((entry = zip.getNextEntry()) != null) {
                        boolean sharedNode = entry.getName().startsWith("node/");
                        if ((sharedNode && nodeInstalled) || (!sharedNode && privateInstalled)) continue;
                        File base = sharedNode ? node : bundled;
                        String relative = sharedNode ? entry.getName().substring(5) : entry.getName();
                        File target = new File(base, relative).getCanonicalFile();
                        if (!target.getPath().startsWith(base.getCanonicalPath() + File.separator)) throw new IOException("Invalid packaged runtime path.");
                        if (entry.isDirectory()) Files.createDirectories(target.toPath());
                        // Preserve the user's shared packages and dependency declarations.
                        else if (!sharedNode || !target.exists()) copy(zip, target);
                    }
                }
                Files.createDirectories(bundled.toPath()); Files.createDirectories(node.toPath());
                Files.write(privateReady.toPath(), version.getBytes(StandardCharsets.UTF_8));
                Files.write(nodeReady.toPath(), version.getBytes(StandardCharsets.UTF_8));
            }
            File nativeDirectory = new File(context.getApplicationInfo().nativeLibraryDir);
            JSONObject extensions = new JSONObject(new String(Files.readAllBytes(new File(bundled, "extensions.json").toPath()), StandardCharsets.UTF_8));
            for (Iterator<String> names = extensions.keys(); names.hasNext();) {
                String name = names.next(), library = extensions.getString(name);
                if (!name.matches("[a-zA-Z0-9_.-]+\\.so") || !library.matches("libpilon_py_[a-zA-Z0-9_.-]+\\.so")) throw new IOException("Invalid extension name.");
                link(new File(nativeDirectory, library), new File(bundled, "python/lib/python3.14/lib-dynload/" + name));
            }
            File bin = new File(privateRoot, "bin");
            for (String name : new String[]{"python", "python3", "node", "npm", "npx", "vite"}) {
                String library = name.startsWith("python") ? "python" : name.equals("node") ? "node" : "node_cli";
                link(new File(nativeDirectory, "libpilon_" + library + ".so"), new File(bin, name));
            }
            return new JSONObject().put("root", bundled.getPath()).put("bin", bin.getPath())
                .put("node", node.getPath()).put("npmrc", new File(privateRoot, "npmrc").getPath()).toString();
        } catch (Exception failure) {
            return "{\"error\":\"Unable to prepare shared project tools. Check workspace access and free space.\"}";
        }
    }
}
