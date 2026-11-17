#!/usr/bin/env python3
"""Regenerate the Android Office format matrix from Pilon's existing test documents."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--office", type=Path, default=ROOT / "runtime/libreoffice/program/soffice")
    parser.add_argument("--python", type=Path, default=ROOT / "runtime/documents/bin/python")
    parser.add_argument("--output", type=Path, default=ROOT / "src/platform/android/tests/assets/office-fixtures.zip")
    args = parser.parse_args()
    formats = {
        "document.docx": [("doc", "MS Word 97"), ("odt", "writer8"), ("rtf", "Rich Text Format"), ("txt", "Text"), ("html", "HTML (StarWriter)")],
        "presentation.pptx": [("ppt", "MS PowerPoint 97"), ("odp", "impress8")],
        "table.xlsx": [("xls", "MS Excel 97"), ("ods", "calc8"), ("csv", "Text - txt - csv (StarCalc)")],
    }
    with tempfile.TemporaryDirectory(prefix="pilon-office-fixtures-") as temporary:
        work = Path(temporary)

        def convert(source, suffix, filter_name, destination):
            destination.mkdir(parents=True, exist_ok=True)
            conversion = suffix + (":" + filter_name if filter_name else "")
            result = subprocess.run([str(args.office), "-env:UserInstallation=" + (work / "profile").as_uri(),
                                     "--headless", "--nologo", "--nodefault", "--norestore", "--convert-to", conversion,
                                     "--outdir", str(destination), str(source)], capture_output=True, text=True, timeout=60)
            output = destination / (source.stem + "." + suffix)
            if result.returncode or not output.is_file():
                raise RuntimeError(f"Fixture conversion failed: {source.name}\n{result.stdout}\n{result.stderr}")
            return output

        inputs = work / "inputs"
        inputs.mkdir()
        for name, targets in formats.items():
            source = ROOT / "src/tests/fixtures" / name
            shutil.copy2(source, inputs / name)
            for suffix, filter_name in targets:
                convert(source, suffix, filter_name, inputs)
        shutil.copy2(inputs / "document.html", inputs / "document.htm")
        (inputs / "table.tsv").write_text("Table sheet 1\t42\nSecond row\t19\n")
        manifest = []
        for source in sorted(inputs.iterdir()):
            pdf = convert(source, "pdf", "", work / "baseline" / source.suffix[1:])
            metadata = subprocess.run([str(args.python), "-c", "import json,pymupdf,sys;d=pymupdf.open(sys.argv[1]);print(json.dumps({'pages':len(d)}))", str(pdf)],
                                      check=True, capture_output=True, text=True)
            expected = json.loads(metadata.stdout)
            expected["filename"] = source.name
            if source.stem == "document": expected["contains"] = ["Document first page", "Document second page"]
            elif source.stem == "presentation": expected["contains"] = ["Slide 1", "Slide 2"]
            else: expected["contains"] = ["Table sheet 1", "42"]
            if source.suffix == ".tsv": expected["contains"] += ["Second row", "19"]
            if source.suffix == ".csv": expected["notContains"] = ["Table sheet 1,42"]
            if source.suffix == ".tsv": expected["notContains"] = ["Table sheet 1\t42"]
            manifest.append(expected)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with ZipFile(args.output, "w", ZIP_DEFLATED) as archive:
            for source in sorted(inputs.iterdir()): archive.write(source, source.name)
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
    print("Generated", len(manifest), "Office fixtures:", args.output)


if __name__ == "__main__":
    main()
