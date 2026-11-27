#!/usr/bin/env python3
"""Create an isolated artifact using Pilon's shared Python and Node runtimes."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def create(kind, name, root, tools):
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", name):
        raise ValueError("Use a short name containing letters, numbers, hyphens or underscores.")
    root, tools = Path(root).resolve(), Path(tools).resolve()
    destination = root / name
    if destination.exists():
        raise ValueError("An artifact with this name already exists.")
    android = sys.platform == "android"
    python = Path(os.environ.get("PILON_PYTHON", sys.executable)) if android else tools / ".venv/bin/python"
    if not python.exists():
        raise ValueError("Run Pilon's setup_agent_workspace.py to prepare the shared Python environment.")
    if kind == "react":
        node = Path(os.environ["PILON_AGENT_NODE_ROOT"]) if android else tools / "node"
        if android:
            if not (node / "node_modules/vite/bin/vite.js").is_file():
                raise ValueError("The shared React tools are missing. Restore the shared dependencies before creating a React artifact.")
        else:
            node.mkdir(exist_ok=True)
            with (node / ".install.lock").open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                manifest = node / "package.json"
                if not manifest.exists():
                    manifest.write_text(json.dumps({"private": True, "dependencies": {
                        "react": "19.1.1", "react-dom": "19.1.1", "vite": "6.4.1"}}, indent=2) + "\n")
                if not (node / "node_modules/vite/bin/vite.js").exists():
                    subprocess.run(["npm", "install", "--prefix", str(node), "--no-audit", "--no-fund"], check=True, stdout=sys.stderr)
    root.mkdir(parents=True, exist_ok=True)
    destination.mkdir()
    try:
        if kind == "python":
            (destination / "main.py").write_text('def main():\n    pass\n\n\nif __name__ == "__main__":\n    main()\n')
            executable = 'os.environ.get("PILON_PYTHON", sys.executable)' if android else repr(str(python))
            (destination / "run").write_text('#!/usr/bin/env python3\nimport os, sys\nfrom pathlib import Path\nos.chdir(Path(__file__).resolve().parent)\npython = ' + executable + '\nos.execv(python, [python, "main.py", *sys.argv[1:]])\n')
        else:
            if not android:
                (destination / "node_modules").symlink_to(node / "node_modules", target_is_directory=True)
            (destination / "package.json").write_text(json.dumps({"name": name.lower(), "private": True, "type": "module", "scripts": {"dev": "vite --host 127.0.0.1", "build": "vite build"}}, indent=2) + "\n")
            (destination / "index.html").write_text('<!doctype html>\n<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"></head><body><div id="root"></div><script type="module" src="/src/main.jsx"></script></body></html>\n')
            (destination / "src").mkdir()
            (destination / "src/main.jsx").write_text('import React from "react";\nimport { createRoot } from "react-dom/client";\n\nfunction App() { return null; }\n\ncreateRoot(document.getElementById("root")).render(<App />);\n')
            # Preserve isolated Vite caches and output even though packages are shared.
            config = 'export default { cacheDir: ".cache/vite", server: { fs: { strict: true } } };\n'
            if android:
                # Android shared storage cannot create node_modules symlinks.
                config = '''import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL(".", import.meta.url));
const shared = process.env.PILON_AGENT_NODE_ROOT || SHARED_ROOT;
export default {
  cacheDir: ".cache/vite",
  server: { fs: { strict: true, allow: [root, shared] } },
  plugins: [{
    name: "pilon-shared-packages", enforce: "pre",
    resolveId(id, importer) {
      if (id.startsWith(".") || id.startsWith("/") || id.startsWith("\\0") || /^[A-Za-z][\\w+.-]*:/.test(id)) return;
      if (importer && !importer.startsWith(root)) return;
      return this.resolve(id, shared + "/package.json", { skipSelf: true });
    }
  }]
};
'''.replace("SHARED_ROOT", json.dumps(str(node)))
            (destination / "vite.config.js").write_text(config)
            (destination / "run").write_text('#!/usr/bin/env python3\nimport os, sys\nfrom pathlib import Path\nos.chdir(Path(__file__).resolve().parent)\nos.execvp("npm", ["npm", "run", "build" if "--build" in sys.argv else "dev", "--", *[arg for arg in sys.argv[1:] if arg != "--build"]])\n')
        if not android:
            (destination / "run").chmod(0o755)
        (destination / ".gitignore").write_text("node_modules/\n.cache/\ndist/\n__pycache__/\n")
    except Exception:
        shutil.rmtree(destination)
        try:
            root.rmdir()
        except OSError:
            pass
        raise
    result = {"path": str(destination), "run": str(destination / "run"), "kind": kind}
    if android:
        result["run_command"] = ["python3", str(destination / "run")]
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=["python", "react"])
    parser.add_argument("name")
    parser.add_argument("--root", required=True, help="This chat's workspace.artifacts from get_app_context")
    parser.add_argument("--tools", default=os.environ.get("PILON_AGENT_TOOLS"), required=not os.environ.get("PILON_AGENT_TOOLS"))
    args = parser.parse_args()
    try:
        print(json.dumps(create(args.kind, args.name, args.root, args.tools)))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + "\n")
