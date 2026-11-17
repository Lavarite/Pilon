package org.pilon;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Uses only owned fixtures to prove public-source Python, React build and live reload. */
public final class AgentRuntimeProbeInstrumentation extends Instrumentation {
    private Bundle arguments;
    @Override public void onCreate(Bundle value) { super.onCreate(value); arguments = value; start(); }
    private File copy(String name, File directory) throws Exception {
        File destination = new File(directory, name);
        try (InputStream input = getContext().getAssets().open("agent-runtime/" + name);
             OutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[65536]; int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
        }
        return destination;
    }
    @Override public void onStart() {
        Bundle output = new Bundle();
        File owned = new File(getTargetContext().getCacheDir(), "agent-runtime-probe-" + UUID.randomUUID());
        java.lang.Process child = null;
        int resultCode = Activity.RESULT_CANCELED;
        try {
            if (!PilonStorage.hasAccess(getTargetContext())) throw new AssertionError("Existing public workspace permission is required; this probe does not request it");
            File workspace = new File(arguments.getString("workspace", PilonStorage.workspacePath())).getCanonicalFile();
            File tools = new File(workspace, ".pilon/assistant/agent/tools");
            JSONObject runtime = new JSONObject(PilonAgentRuntime.prepare(getTargetContext(), tools.getPath()));
            if (runtime.has("error")) throw new AssertionError(runtime.getString("error"));
            if (!owned.mkdir()) throw new IOException("Cannot create owned probe directory");
            File script = copy("android_agent_runtime_smoke.py", owned), creator = copy("create_artifact.py", owned);
            File root = new File(workspace, "Artifacts");
            if (!root.isDirectory() && !root.mkdirs()) throw new IOException("Cannot access public artifacts directory");
            String libraries = getTargetContext().getApplicationInfo().nativeLibraryDir;
            String bin = runtime.getString("bin"), bundle = runtime.getString("root"), node = runtime.getString("node");
            ProcessBuilder builder = new ProcessBuilder(bin + "/python3", script.getPath(), "--creator", creator.getPath(), "--root", root.getPath());
            if (arguments.getBoolean("diagnose", false) || "true".equals(arguments.getString("diagnose"))) builder.command().add("--diagnose-esbuild");
            Map<String, String> environment = builder.environment();
            if ("true".equals(arguments.getString("diagnose"))) {
                String probeLibraries = getContext().getApplicationInfo().nativeLibraryDir;
                for (String[] entry : new String[][]{{"123","go1.23.12"},{"127","go1.27.3"}}) {
                    File probe = new File(probeLibraries, "libpilon_gofs_" + entry[1] + ".so");
                    if (probe.isFile()) environment.put("PILON_GO_FS_PROBE_" + entry[0], probe.getPath());
                }
            }
            environment.put("HOME", getTargetContext().getFilesDir().getPath());
            environment.put("SHELL", "/system/bin/sh");
            environment.put("PATH", bin + ":/system/bin");
            environment.put("PYTHONHOME", bundle + "/python");
            environment.put("PYTHONUTF8", "1");
            environment.put("PILON_PYTHON", bin + "/python3");
            environment.put("PYTHONPATH", tools.getPath() + "/python-packages");
            environment.put("PIP_TARGET", tools.getPath() + "/python-packages");
            environment.put("PIP_CACHE_DIR", tools.getPath() + "/pip-cache");
            environment.put("PILON_NODE", bin + "/node");
            environment.put("PILON_AGENT_TOOLS", tools.getPath());
            environment.put("PILON_AGENT_NODE_ROOT", node);
            environment.put("PILON_NPM_CLI", bundle + "/npm/bin/npm-cli.js");
            environment.put("PILON_NPX_CLI", bundle + "/npm/bin/npx-cli.js");
            environment.put("PILON_VITE_CLI", node + "/node_modules/vite/bin/vite.js");
            environment.put("NODE_PATH", node + "/node_modules");
            environment.put("ESBUILD_BINARY_PATH", libraries + "/libpilon_esbuild.so");
            if ("true".equals(arguments.getString("patchedEsbuild"))) {
                File patched = new File(getContext().getApplicationInfo().nativeLibraryDir, "libpilon_esbuild_probe.so");
                if (!patched.isFile()) throw new AssertionError("The isolated patched esbuild helper is missing");
                environment.put("ESBUILD_BINARY_PATH", patched.getPath());
            }
            environment.put("NPM_CONFIG_BIN_LINKS", "false");
            environment.put("NPM_CONFIG_UPDATE_NOTIFIER", "false");
            environment.put("NPM_CONFIG_USERCONFIG", runtime.getString("npmrc"));
            environment.put("LD_LIBRARY_PATH", libraries);
            File log = new File(owned, "result.log");
            builder.redirectErrorStream(true).redirectOutput(log);
            child = builder.start();
            if (!child.waitFor(180, TimeUnit.SECONDS)) throw new AssertionError("Runtime probe exceeded 180 seconds");
            String result = new String(Files.readAllBytes(log.toPath()), StandardCharsets.UTF_8);
            if (child.exitValue() != 0) throw new AssertionError(result);
            JSONObject proof = new JSONObject(result.trim());
            if (!proof.has("scope") && (!proof.getBoolean("react_build_passed") || !proof.getBoolean("publicWorkspaceVerified") || !proof.getBoolean("source_edit_preserved"))) throw new AssertionError("Public workspace proof is incomplete");
            output.putString("stream", proof.toString() + "\n");
            resultCode = Activity.RESULT_OK;
        } catch (Throwable failure) {
            output.putString("stream", "Agent runtime proof failed: " + failure + "\n");

        } finally {
            if (child != null && child.isAlive()) child.destroyForcibly();
            if (owned.isDirectory()) {
                try (java.util.stream.Stream<java.nio.file.Path> files = Files.walk(owned.toPath())) {
                    files.sorted(java.util.Comparator.reverseOrder()).forEach(path -> path.toFile().delete());
                } catch (IOException ignored) { /* Only this UUID fixture is eligible for cleanup. */ }
            }
        }
        finish(resultCode, output);
    }
}
