"""Decode PDF data for Pilon's custom viewer; no UI or input handling lives here."""
import json
import sys

import pymupdf


def rect_values(rect):
    return [rect.x0, rect.y0, rect.width, rect.height]


def page_text(page):
    text, boxes = [], []
    matrix = page.rotation_matrix
    for block in page.get_text("rawdict", sort=True)["blocks"]:
        for line in block.get("lines", []):
            for span in line["spans"]:
                for char in span["chars"]:
                    value = char["c"]
                    text.append(value)
                    # QString offsets are UTF-16, including surrogate pairs.
                    box = rect_values(pymupdf.Rect(char["bbox"]) * matrix)
                    boxes.extend([box] * (len(value.encode("utf-16-le")) // 2))
            text.append("\n")
            boxes.append([0, 0, 0, 0])
    return {"text": "".join(text), "characters": boxes}


def sections(document):
    roots, parents = [], []
    for level, title, number, target in document.get_toc(simple=False):
        page = target.get("page", number - 1)
        point = target.get("to", pymupdf.Point(0, 0))
        # Named destinations from get_toc retain PDF bottom-left coordinates.
        # Direct GoTo destinations are already in MuPDF page coordinates.
        if target.get("kind") == pymupdf.LINK_NAMED and 0 <= page < len(document):
            point = point * document[page].transformation_matrix * document[page].rotation_matrix
        top = point.y
        item = {"title": title, "page": page, "top": top / document[page].rect.height if 0 <= page < len(document) else 0, "children": []}
        while parents and parents[-1][0] >= level:
            parents.pop()
        (parents[-1][1]["children"] if parents else roots).append(item)
        parents.append((level, item))
    return roots


def process(request):
    with pymupdf.open(request["source"]) as document:
        if document.needs_pass:
            raise ValueError("This PDF requires a password.")
        command = request["command"]
        if command == "open":
            return {"pages": [{"width": page.rect.width, "height": page.rect.height} for page in document], "sections": sections(document)}
        if command == "render":
            page = document[request["page"]]
            scale = request["resolution"] / max(page.rect.width, page.rect.height)
            page.get_pixmap(matrix=pymupdf.Matrix(scale, scale), alpha=False).save(request["output"])
            result = page_text(page)
            result["links"] = []
            for link in page.get_links():
                if link["kind"] != pymupdf.LINK_GOTO or link.get("page", -1) < 0:
                    continue
                area = link["from"] * page.rotation_matrix
                target = document[link["page"]]
                result["links"].append({"page": link["page"], "top": link.get("to", pymupdf.Point()).y / target.rect.height,
                    "area": [area.x0 / page.rect.width, area.y0 / page.rect.height, area.width / page.rect.width, area.height / page.rect.height]})
            return result
        if command == "search":
            if request["all"]:
                return {"pages": [page_text(page) for page in document], "all": True}
            backwards, needle = request["backwards"], request["text"]
            # Python regex preserves original string offsets for Unicode matches.
            import re
            pattern = re.compile(re.escape(needle), re.IGNORECASE)
            for offset in range(len(document) + 1):
                number = (request["from"] + (-offset if backwards else offset)) % len(document)
                result = page_text(document[number])
                matches = list(pattern.finditer(result["text"]))
                if backwards:
                    matches.reverse()
                for match in matches:
                    start = len(result["text"][:match.start()].encode("utf-16-le")) // 2
                    end = len(result["text"][:match.end()].encode("utf-16-le")) // 2
                    if offset == 0 and request["continuation"]:
                        if backwards and start >= request["anchor"]:
                            continue
                        if not backwards and start < request["cursor"]:
                            continue
                    return {"page": number, "offset": start, "length": end - start, "pages": [result]}
            return {"page": -1}
        raise ValueError("Unknown document operation.")


if __name__ == "__main__":
    pymupdf.TOOLS.mupdf_display_errors(False)
    pymupdf.TOOLS.mupdf_display_warnings(False)
    try:
        result = process(json.load(sys.stdin))
    except Exception as error:
        result = {"error": str(error)}
    print(json.dumps(result, ensure_ascii=True))
