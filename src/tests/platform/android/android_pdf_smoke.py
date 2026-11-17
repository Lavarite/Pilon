#!/usr/bin/env python3
"""Compile and exercise Pilon's PDF decoder/importer on an authorized USB phone."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shlex
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[4]


def run(*command, **kwargs):
    result = subprocess.run([str(part) for part in command], capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed: {command}\n{result.stdout!s}\n{result.stderr!s}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", type=Path, default=Path.home() / "Android/Sdk/platform-tools/adb")
    parser.add_argument("--qt", type=Path, default=Path.home() / "Qt/6.12.0/android_arm64_v8a")
    parser.add_argument("--host-qt", type=Path, default=Path.home() / "Qt/6.12.0/gcc_64")
    parser.add_argument("--sdk", type=Path, default=Path.home() / "Android/Sdk")
    parser.add_argument("--ndk", type=Path, default=Path.home() / "Android/Sdk/ndk/27.2.12479018")
    parser.add_argument("--python", type=Path, default=ROOT / "runtime/documents/bin/python")
    args = parser.parse_args()
    work = ROOT / "build/android/pdfium"
    source, build, fixtures = work / "probe-source", work / "smoke-build", work / "fixtures"
    for folder in (source, fixtures):
        folder.mkdir(parents=True, exist_ok=True)
    (source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.22)
project(PilonPdfProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Concurrent)
add_executable(pdf-probe "{ROOT}/src/tests/core/android_pdf_probe.cpp" "{ROOT}/src/core/documents/documentpreview.cpp" "{ROOT}/src/core/documents/documentpreview.h" "{ROOT}/src/core/documents/noteimport.cpp")
target_include_directories(pdf-probe PRIVATE "{ROOT}/src/core/documents" "{work}/sdk/include")
target_link_libraries(pdf-probe PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Concurrent "{work}/sdk/lib/libpdfium.so")
target_link_options(pdf-probe PRIVATE -Wl,-z,max-page-size=16384)
''')
    configure = run(args.qt / "bin/qt-cmake", "-S", source, "-B", build, "-G", "Ninja",
                    f"-DANDROID_SDK_ROOT={args.sdk}", f"-DANDROID_NDK_ROOT={args.ndk}",
                    "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", f"-DQT_HOST_PATH={args.host_qt}")
    (work / "probe-configure.log").write_bytes(configure.stdout + configure.stderr)
    compile_result = run("cmake", "--build", build, "-j4")
    (work / "probe-build.log").write_bytes(compile_result.stdout + compile_result.stderr)
    # The host runtime generates a real embedded-font PDF, including UTF-16 surrogate mapping.
    generate = '''from pathlib import Path
import pymupdf,sys
out=Path(sys.argv[1]); doc=pymupdf.open()
for i in range(3):
 p=doc.new_page(width=400,height=600)
 p.insert_font(fontname="pilon",fontfile="/usr/share/fonts/TTF/DejaVuSans.ttf")
 for y,text,size in [(70,f"Page {i+1} selectable text",18),(110,"Another selectable text match",16),(150,"Unicode café 😀 after",16)]:
  p.insert_text((40,y),text,fontname="pilon",fontsize=size)
 if i==1:p.set_rotation(90)
 if i==2:p.set_cropbox(pymupdf.Rect(20,20,380,580))
doc.set_toc([[1,"First page",1],[2,"Rotated page",2],[1,"Cropped page",3]])
p=doc[0]
p.insert_link({"kind":pymupdf.LINK_GOTO,"from":pymupdf.Rect(35,50,260,80),"page":1,"to":pymupdf.Point(0,90)})
w=pymupdf.Widget();w.field_name="test-field";w.field_type=pymupdf.PDF_WIDGET_TYPE_TEXT
w.rect=pymupdf.Rect(40,200,280,240);w.field_value="Form value visible";w.text_font="Helv";w.text_fontsize=16
p.add_widget(w)
for xref in range(1,doc.xref_length()):
 if doc.xref_is_stream(xref):
  stream=doc.xref_stream(xref)
  if b"<1f600>" in stream:doc.update_stream(xref,stream.replace(b"<1f600>",b"<d83dde00>"))
doc.save(out/"unicode-rotated.pdf")
doc.save(out/"password.pdf",encryption=pymupdf.PDF_ENCRYPT_AES_256,owner_pw="owner",user_pw="test")
(out/"invalid.pdf").write_bytes(b"not a PDF")
destinations=pymupdf.open();page=destinations.new_page(width=400,height=600)
destinations.set_toc([[1,name,1] for name in ("FitH","FitBH","FitR")])
for item,view in zip(destinations.get_toc(False),( "FitH 450", "FitBH 450", "FitR 0 0 400 450")):
 destinations.xref_set_key(item[3]["xref"],"A",f"<< /S /GoTo /D [{page.xref} 0 R /{view}] >>")
destinations.save(out/"destinations.pdf")
heavy=pymupdf.open();page=heavy.new_page(width=400,height=600)
page.draw_rect(pymupdf.Rect(0,0,1,1))
content="0 0 0 rg\\n"+"".join(f"{i%300} {i%500} 100 100 re f\\n" for i in range(100000))
heavy.update_stream(page.get_contents()[0],content.encode());heavy.save(out/"heavy.pdf")
'''
    run(args.python, "-c", generate, fixtures)
    libraries = [build / "pdf-probe", ROOT / "builds/android/runtime/arm64-v8a/libpdfium.so",
                 args.ndk / "toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"]
    libraries += [args.qt / f"lib/libQt6{name}_arm64-v8a.so" for name in ("Core", "Gui", "Qml", "Network", "Concurrent")]
    for library in libraries:
        segments = [row.split() for row in run("readelf", "-lW", library, text=True).stdout.splitlines() if row.strip().startswith("LOAD")]
        assert segments and all(int(row[-1], 16) >= 16384 for row in segments), library

    def adb(*arguments, **kwargs):
        return run(args.adb, "-d", *arguments, **kwargs)

    remote = "/data/local/tmp/pilon-pdf-probe-" + uuid.uuid4().hex
    proof = {"scope": "production Android decoder/importer under isolated shell UID; installed app interactions remain separate", "replies": {}}
    try:
        adb("shell", "mkdir", "-p", remote + "/tmp", remote + "/note")
        for path in [*libraries, *fixtures.glob("*.pdf"), *(ROOT / "src/tests/fixtures" / name for name in ("viewer.pdf", "named-section.pdf"))]:
            adb("push", path, remote + "/" + path.name)
        adb("shell", "chmod", "700", remote + "/pdf-probe")
        program = f"env LD_LIBRARY_PATH={shlex.quote(remote)} {shlex.quote(remote + '/pdf-probe')} {shlex.quote(remote + '/tmp')}"

        def request(name, **values):
            response = adb("shell", program, input=json.dumps(values) + "\n", text=True, timeout=120)
            result = json.loads(response.stdout)
            proof["replies"][name] = result
            return result

        def document(name, command, **values):
            return request(name + "-" + command + "-" + str(len(proof["replies"])), source=remote + "/" + name, command=command, **values)

        opened = document("viewer.pdf", "open")
        assert opened["pages"] == [{"width": 400, "height": 600}, {"width": 600, "height": 400}], opened
        assert len(opened["sections"]) == 2 and opened["sections"][0]["children"][0]["children"][0]["title"] == "Nested subsection", opened
        named = document("named-section.pdf", "open")
        assert named["sections"][0]["page"] == 0 and named["sections"][0]["top"] == .25, named
        fitted = document("destinations.pdf", "open")
        assert [section["top"] for section in fitted["sections"]] == [.25, .25, .25], fitted
        geometry = document("unicode-rotated.pdf", "open")["pages"]
        assert geometry == [{"width": 400, "height": 600}, {"width": 600, "height": 400}, {"width": 360, "height": 560}], geometry
        for number, size in enumerate(geometry):
            rendered = document("unicode-rotated.pdf", "render", page=number, resolution=1800, output=remote + f"/page-{number}.png", pageWidth=size["width"], pageHeight=size["height"])
            text, boxes = rendered["text"], rendered["characters"]
            assert "Unicode café 😀 after" in text and len(boxes) == len(text.encode("utf-16-le")) // 2, rendered
            emoji = len(text[:text.index("😀")].encode("utf-16-le")) // 2
            assert boxes[emoji] == boxes[emoji + 1] and boxes[emoji][2] > 0, rendered
            assert rendered["testImage"]["firstCharacterHasInk"] and max(rendered["testImage"]["width"], rendered["testImage"]["height"]) == 1800, rendered
            assert all(len(box) == 4 and all(math.isfinite(value) for value in box) for box in boxes), rendered
            adb("pull", remote + f"/page-{number}.png", work / f"phone-page-{number}.png")
        link = proof["replies"][next(key for key in proof["replies"] if "unicode-rotated.pdf-render" in key)]["links"][0]
        assert link["page"] == 1 and link["area"] == [.0875, 1 / 12, .5625, .05], link
        search = dict(all=False, backwards=False, continuation=False, text="selectable", **{"from": 0})
        first = document("unicode-rotated.pdf", "search", **search)
        assert first["page"] == 0 and first["length"] == 10, first
        again = document("unicode-rotated.pdf", "search", **dict(search, continuation=True, anchor=first["offset"], cursor=first["offset"] + first["length"]))
        assert again["page"] == 0 and again["offset"] > first["offset"], again
        previous = document("unicode-rotated.pdf", "search", **dict(search, backwards=True, continuation=True, anchor=again["offset"], cursor=again["offset"] + again["length"]))
        assert previous["offset"] == first["offset"], previous
        emoji_match = document("unicode-rotated.pdf", "search", **dict(search, text="😀 AFTER"))
        assert emoji_match["length"] == 8, emoji_match
        all_text = document("unicode-rotated.pdf", "search", all=True)
        assert all_text["all"] and len(all_text["pages"]) == 3, all_text
        for name in ("password.pdf", "invalid.pdf", "missing.pdf"):
            assert document(name, "open").get("error"), name
        assert document("viewer.pdf", "render", page=99).get("error")
        assert document("viewer.pdf", "open", cancelled=True).get("error")
        # Preserve an existing image if a different request is cancelled mid-render.
        existing = adb("shell", "sha256sum", remote + "/page-0.png", text=True).stdout.split()[0]
        interrupted = document("heavy.pdf", "render", page=0, resolution=3600, output=remote + "/page-0.png", cancelAfterMs=30)
        assert interrupted.get("error") and "cancel" in interrupted["error"].lower(), interrupted
        assert adb("shell", "sha256sum", remote + "/page-0.png", text=True).stdout.split()[0] == existing
        concurrent = request("concurrent-search", command="concurrent", source=remote + "/heavy.pdf", searchSource=remote + "/viewer.pdf", output=remote + "/heavy.png")
        assert concurrent["page"] == 0 and concurrent["renderStillRunning"] and concurrent["searchMilliseconds"] < 500, concurrent
        encoding_cancel = document("viewer.pdf", "render", page=0, resolution=3600, output=remote + "/page-0.png", cancelOnSave=True)
        assert encoding_cancel.get("error") and encoding_cancel["saveObserved"], encoding_cancel
        assert adb("shell", "sha256sum", remote + "/page-0.png", text=True).stdout.split()[0] == existing
        imported = request("import", command="import", sources=[remote + "/unicode-rotated.pdf"], destination=remote + "/note", job="success")
        assert len(imported["elements"]) == 3 and all(page["type"] == "page" and page["width"] == 600 for page in imported["elements"]), imported
        hashes = adb("shell", "sha256sum", remote + "/unicode-rotated.pdf", text=True).stdout.split()[0]
        assert hashes == hashlib.sha256((fixtures / "unicode-rotated.pdf").read_bytes()).hexdigest()
        failed = request("import-failure", command="import", sources=[remote + "/viewer.pdf", remote + "/invalid.pdf"], destination=remote + "/note", job="failure")
        assert failed.get("error"), failed
        cancelled = request("import-cancel", command="import", sources=[remote + "/viewer.pdf"], destination=remote + "/note", job="cancel", cancelled=True)
        assert cancelled.get("error"), cancelled
        assert adb("shell", f"test ! -e {remote}/note/assets/failure && test ! -e {remote}/note/assets/cancel && test -f {remote}/note/assets/success/000001.png").returncode == 0
        proof["passed"] = ["16KB alignment", "page sizes and nested bookmarks", "named and FitH/FitBH/FitR destinations", "fonts and forms rendering", "rotated/cropped character geometry", "UTF-16 surrogate boxes", "forward/backward/continued Unicode search", "all text selection data", "internal links", "invalid/password/cancel errors", "mid-render cancellation preserves existing PNG", "search stays responsive during heavy rendering", "cancel during PNG encoding preserves existing output", "PDF note import", "source preservation and failed/cancelled import rollback"]
    finally:
        cleanup = subprocess.run([str(args.adb), "-d", "shell", "rm", "-rf", remote], capture_output=True)
        proof["cleaned"] = cleanup.returncode == 0
        (work / "device-proof.json").write_text(json.dumps(proof, indent=2) + "\n")
        assert cleanup.returncode == 0, remote
    print("Android PDF checks passed:", ", ".join(proof["passed"]))
    print("Evidence:", work / "device-proof.json")


if __name__ == "__main__":
    main()
