#!/usr/bin/env python3
"""Execute Qt's actual Enter dispatch method with isolated view/listener test doubles."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def verify(source, jdk, android_jar):
    text = source.read_text()
    signature = "public boolean sendKeyEvent(KeyEvent event)"
    assert text.count(signature) == 1
    start = text.index(signature)
    position = text.index("{", start)
    depth = 1
    while depth:
        position += 1
        depth += (text[position] == "{") - (text[position] == "}")
    method = text[start:position + 1]
    # Only the surrounding platform boundary is doubled. Dispatch executes the
    # source method unchanged, against constants from the public Android SDK.
    harness = r'''
import android.view.inputmethod.EditorInfo;
class KeyEvent {
    static final int KEYCODE_ENTER=66, KEYCODE_TAB=61, META_SHIFT_ON=1;
    final long down, time; final int action, code, repeat, meta;
    KeyEvent(long d,long t,int a,int c,int r,int m) {down=d;time=t;action=a;code=c;repeat=r;meta=m;}
    long getDownTime(){return down;} long getEventTime(){return time;}
    int getAction(){return action;} int getKeyCode(){return code;}
    int getRepeatCount(){return repeat;} int getMetaState(){return meta;}
}
class BaseInputConnection {
    KeyEvent sent; int forwarded;
    public boolean sendKeyEvent(KeyEvent event) {sent=event;++forwarded;return true;}
}
class QtInputConnection extends BaseInputConnection {
    static class View {int m_imeOptions; View(int options){m_imeOptions=options;}}
    final View m_view; int hides, restarts, finishes;
    class Listener {void onSendKeyEventDefaultCase(){++hides;}}
    final Listener m_qtInputConnectionListener=new Listener();
    QtInputConnection(int options){m_view=new View(options);}
    void finishComposingText(){++finishes;} void restartImmInput(){++restarts;}
    @Override
    /* SOURCE_METHOD */
}
public class ImeDispatchCheck {
    static int checks;
    static void require(boolean value,String message){if(!value)throw new AssertionError(message);++checks;}
    static void check(int options,int hides,int restarts,int code,int meta) {
        QtInputConnection connection=new QtInputConnection(options);
        for(int action:new int[]{0,1}) {
            KeyEvent original=new KeyEvent(10,20,action,KeyEvent.KEYCODE_ENTER,2,0);
            require(connection.sendKeyEvent(original),"Event rejected");
            require(connection.sent.getKeyCode()==code,"Wrong forwarded key for "+options);
            require(connection.sent.getAction()==action,"Key edge changed");
            require(connection.sent.getMetaState()==meta,"Modifiers changed");
            require(connection.sent.getRepeatCount()==2,"Repeat count changed");
        }
        require(connection.hides==hides,"Wrong hide count for "+options+": "+connection.hides);
        require(connection.restarts==restarts,"Wrong restart count for "+options);
        require(connection.finishes==2 && connection.forwarded==2,"Composition or forwarding changed");
        require(connection.m_view.m_imeOptions==options,"Editor flags were modified");
    }
    public static void main(String[] args) {
        int noEnter=EditorInfo.IME_FLAG_NO_ENTER_ACTION, noFullscreen=EditorInfo.IME_FLAG_NO_FULLSCREEN;
        check(noEnter|noFullscreen,0,2,KeyEvent.KEYCODE_ENTER,0);
        check(noEnter,0,2,KeyEvent.KEYCODE_ENTER,0);
        check(EditorInfo.IME_ACTION_DONE|noFullscreen,2,0,KeyEvent.KEYCODE_ENTER,0);
        check(EditorInfo.IME_ACTION_DONE,2,0,KeyEvent.KEYCODE_ENTER,0);
        check(EditorInfo.IME_ACTION_NEXT|noFullscreen,0,0,KeyEvent.KEYCODE_TAB,0);
        check(EditorInfo.IME_ACTION_PREVIOUS|noFullscreen,0,0,KeyEvent.KEYCODE_TAB,KeyEvent.META_SHIFT_ON);
        check(noEnter|noFullscreen|EditorInfo.IME_ACTION_DONE,0,2,KeyEvent.KEYCODE_ENTER,0);
        QtInputConnection connection=new QtInputConnection(noEnter|noFullscreen);
        KeyEvent other=new KeyEvent(1,2,0,29,0,0);
        require(connection.sendKeyEvent(other) && connection.sent==other,"Non-Enter event changed");
        require(connection.hides==0 && connection.restarts==0 && connection.finishes==1,"Non-Enter dispatch changed");
        System.out.println("Qt Android IME dispatch checks passed: "+checks);
    }
}
'''.replace("/* SOURCE_METHOD */", method)
    with tempfile.TemporaryDirectory(prefix="pilon-qt-ime-") as directory:
        temporary = Path(directory)
        java = temporary / "ImeDispatchCheck.java"
        java.write_text(harness)
        subprocess.run([str(jdk / "bin/javac"), "--release", "17", "-classpath", str(android_jar),
                        "-d", str(temporary), str(java)], check=True)
        subprocess.run([str(jdk / "bin/java"), "-cp", str(temporary), "ImeDispatchCheck"], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--jdk", type=Path, default=Path(os.environ.get("JAVA_HOME", str(Path.home() / "Android/jdk-21"))))
    parser.add_argument("--android-jar", type=Path, default=Path.home() / "Android/Sdk/platforms/android-37.0/android.jar")
    arguments = parser.parse_args()
    verify(arguments.source, arguments.jdk, arguments.android_jar)
