#!/usr/bin/env python3
"""Execute QtTextureView callbacks with queued native-consumer test doubles."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def verify(source, jdk):
    text = source.read_text()
    methods = []
    for signature in ("public void onSurfaceTextureAvailable(",
                      "public void onSurfaceTextureSizeChanged(",
                      "public boolean onSurfaceTextureDestroyed("):
        assert text.count(signature) == 1
        start = text.index(signature)
        position = text.index("{", start)
        depth = 1
        while depth:
            position += 1
            depth += (text[position] == "{") - (text[position] == "}")
        methods.append(text[start:position + 1])
    # Execute the production callbacks unchanged. The double only records the
    # queued native lifecycle and resource lifetime; it does not emulate EGL.
    harness = r'''
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
class SurfaceTexture {
    int releases;
    void release() { ++releases; }
}
class Surface {
    final SurfaceTexture texture; int releases;
    Surface(SurfaceTexture value) { texture=value; }
    void release() { ++releases; }
}
class Consumer {
    final ArrayDeque<Runnable> pending=new ArrayDeque<>();
    final List<String> events=new ArrayList<>();
    Surface nativeSurface;
    void onSurfaceCreated(Surface surface) {
        events.add("create");
        pending.add(() -> { ResizeCheck.require(nativeSurface==null,"Recreated before native teardown"); nativeSurface=surface; });
    }
    void onSurfaceDestroyed(Runnable released) {
        events.add("destroy");
        pending.add(() -> { nativeSurface=null; released.run(); });
    }
    void onSurfaceRedrawNeeded(Runnable finished) { events.add("redraw"); pending.add(finished); }
    void drain() { while(!pending.isEmpty()) pending.remove().run(); }
}
class QtTextureView {
    final Consumer m_surfaceCallback=new Consumer();
    Surface m_surface;
    /* SOURCE_METHODS */
}
public class ResizeCheck {
    static int checks;
    static void require(boolean condition,String message) { if(!condition)throw new AssertionError(message); ++checks; }
    public static void main(String[] args) {
        QtTextureView view=new QtTextureView();
        SurfaceTexture texture=new SurfaceTexture();
        view.onSurfaceTextureSizeChanged(texture,1080,2520);
        Surface original=view.m_surface;
        view.m_surfaceCallback.drain();
        require(view.m_surfaceCallback.nativeSurface==original,"Initial Surface missing");
        view.onSurfaceTextureAvailable(texture,1080,2520);
        require(view.m_surfaceCallback.events.size()==1,"Initial Surface duplicated");

        // The parent remains alive under a child window; native callbacks may
        // stay queued until after its Android TextureView has already resized.
        view.m_surfaceCallback.events.clear();
        view.onSurfaceTextureSizeChanged(texture,2256,2504);
        require(view.m_surfaceCallback.events.equals(List.of("destroy","create")),"Resize retained the old GPU surface");
        require(view.m_surface==original,"Resize replaced the Java Surface");
        require(texture.releases==0 && original.releases==0,"Resize released the producer");
        view.m_surfaceCallback.drain();
        require(view.m_surfaceCallback.nativeSurface==original,"Resized parent was not re-exposed");

        // Rapid IME/rotation resizes followed by real detach must retain the
        // producer until every queued native consumer has finished with it.
        view.onSurfaceTextureSizeChanged(texture,2256,1600);
        view.onSurfaceTextureSizeChanged(texture,2504,2256);
        require(!view.onSurfaceTextureDestroyed(texture),"Android released the texture early");
        require(view.m_surface==null,"Destroyed Surface retained in view");
        require(texture.releases==0 && original.releases==0,"Producer released before queued native teardown");
        SurfaceTexture replacementTexture=new SurfaceTexture();
        view.onSurfaceTextureAvailable(replacementTexture,2504,2256);
        Surface replacement=view.m_surface;
        view.m_surfaceCallback.drain();
        require(texture.releases==1 && original.releases==1,"Genuine destruction did not release exactly once");
        require(replacementTexture.releases==0 && replacement.releases==0,"Replacement producer was released");
        require(view.m_surfaceCallback.nativeSurface==replacement,"Rapid detach/recreate lost the new Surface");
        System.out.println("Qt Android TextureView resize lifecycle checks passed: "+checks);
    }
}
'''.replace("/* SOURCE_METHODS */", "\n".join(methods))
    with tempfile.TemporaryDirectory(prefix="pilon-qt-texture-resize-") as directory:
        temporary = Path(directory)
        java = temporary / "ResizeCheck.java"
        java.write_text(harness)
        subprocess.run([str(jdk / "bin/javac"), "--release", "17", "-d", str(temporary), str(java)], check=True)
        subprocess.run([str(jdk / "bin/java"), "-cp", str(temporary), "ResizeCheck"], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--jdk", type=Path, default=Path(os.environ.get("JAVA_HOME", str(Path.home() / "Android/jdk-21"))))
    arguments = parser.parse_args()
    verify(arguments.source, arguments.jdk)
