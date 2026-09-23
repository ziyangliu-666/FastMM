"""MkDocs hooks for the FastMM site (mkdocs.yml `hooks:`).

Three jobs:

  * `on_page_markdown` turns a relative link that leaves `docs/` (to a test, an example or
    `CONTRIBUTING.md`) into a link to the same file on GitHub, so the pages stay browsable on
    GitHub and `tools/docs_links.py --check` keeps passing while the site has no dead links;
  * `on_post_build` runs Doxygen over the public headers and copies the result into
    `site/api/cpp/`, next to the hand-written `docs/api/cpp.md` page that indexes it;
  * it then checks that every `href` on that page exists in the Doxygen output, because MkDocs
    does not see inside raw HTML.

Environment:
  FASTMM_DOCS_CPP_API=0   skip the Doxygen build (the C++ pages 404; used by `mkdocs serve`)
  DOXYGEN=/path/doxygen   the Doxygen binary (default: `doxygen` on PATH)
"""
from __future__ import annotations

import logging
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlsplit

log = logging.getLogger("mkdocs.hooks.fastmm")

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_DIR = ROOT / "include"
MANIFEST = ROOT / "docs" / "api" / "public-headers.txt"
DOXY_DIR = ROOT / "doxygen"
CACHE_DIR = ROOT / "build" / "docs" / "cpp"
CPP_PAGE = ROOT / "docs" / "api" / "cpp.md"

FENCE = re.compile(r"^\s*(```+|~~~+)")
INLINE_CODE = re.compile(r"(`+)(?:(?!\1).)+?\1")
LINK = re.compile(r"(!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*)(<[^>]*>|[^)\s]+)((?:\s+\"[^\"]*\")?\s*\))")
REF_DEF = re.compile(r"^(\s{0,3}\[[^\]]+\]:\s*)(<[^>]*>|\S+)(\s*)$")
HREF = re.compile(r"href=\"([^\"]+)\"")

# ---------------------------------------------------------------------------- links out of docs/


def _rewrite_target(target: str, page_dir: Path, docs_dir: Path, repo_url: str) -> str | None:
    """The GitHub URL for a relative link that leaves docs/, or None to leave the link alone."""
    bare = target[1:-1] if target.startswith("<") else target
    if not bare or urlsplit(bare).scheme or bare.startswith(("#", "/")):
        return None
    path_part, _, fragment = bare.partition("#")
    if not path_part:
        return None
    dest = (page_dir / path_part).resolve()
    if dest.is_relative_to(docs_dir) or not dest.is_relative_to(ROOT):
        return None
    rel = dest.relative_to(ROOT).as_posix()
    kind = "tree" if dest.is_dir() else "blob"
    url = f"{repo_url.rstrip('/')}/{kind}/main/{rel}"
    return url + (f"#{fragment}" if fragment else "")


def on_page_markdown(markdown: str, page, config, files) -> str:  # noqa: ANN001, ARG001
    repo_url = config.get("repo_url")
    if not repo_url:
        return markdown
    docs_dir = Path(config["docs_dir"]).resolve()
    page_dir = (docs_dir / page.file.src_uri).resolve().parent
    out: list[str] = []
    fence: str | None = None
    for line in markdown.splitlines():
        m = FENCE.match(line)
        if fence is not None:
            if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence):
                fence = None
            out.append(line)
            continue
        if m:
            fence = m.group(1)
            out.append(line)
            continue
        spans = [(c.start(), c.end()) for c in INLINE_CODE.finditer(line)]

        def in_code(pos: int, spans: list[tuple[int, int]] = spans) -> bool:
            return any(a <= pos < b for a, b in spans)

        def repl(mo: re.Match[str]) -> str:
            if in_code(mo.start()):
                return mo.group(0)
            url = _rewrite_target(mo.group(2), page_dir, docs_dir, repo_url)
            return mo.group(0) if url is None else f"{mo.group(1)}{url}{mo.group(3)}"

        line = LINK.sub(repl, line)
        d = REF_DEF.match(line)
        if d and not in_code(d.start(2)):
            url = _rewrite_target(d.group(2), page_dir, docs_dir, repo_url)
            if url is not None:
                line = f"{d.group(1)}{url}{d.group(3)}"
        out.append(line)
    return "\n".join(out) + ("\n" if markdown.endswith("\n") else "")


# ------------------------------------------------------------------------------- C++ (Doxygen)


def _public_headers() -> list[Path]:
    headers: list[Path] = []
    for raw in MANIFEST.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        path = INCLUDE_DIR / fields[-1]
        if not path.is_file():
            raise FileNotFoundError(f"{MANIFEST}: {fields[-1]}: no such header")
        headers.append(path)
    return headers


def _version() -> str:
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(fastmm\s+VERSION\s+([0-9.]+)", text)
    return m.group(1) if m else "0"


def _inputs(headers: list[Path]) -> list[Path]:
    extra = [MANIFEST, DOXY_DIR / "Doxyfile.in", DOXY_DIR / "custom.css", Path(__file__),
             ROOT / "tools" / "doxygen_filter.py"]
    return headers + extra + sorted(DOXY_DIR.glob("awesome/*"))


def _stale(html: Path, inputs: list[Path]) -> bool:
    stamp = html / "index.html"
    if not stamp.is_file():
        return True
    newest = max(p.stat().st_mtime for p in inputs)
    return newest > stamp.stat().st_mtime


def _write_header(doxygen: str, doxyfile: Path, dest: Path) -> None:
    """Doxygen's own header template with the doxygen-awesome scripts added (version-proof)."""
    tmp = dest.parent / "_template"
    tmp.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [doxygen, "-w", "html", str(tmp / "header.html"), str(tmp / "footer.html"),
         str(tmp / "style.css"), str(doxyfile)],
        check=True, cwd=ROOT, stdout=subprocess.DEVNULL,
    )
    scripts = "\n".join(
        f'<script type="text/javascript" src="$relpath^{name}"></script>'
        for name in ("doxygen-awesome-darkmode-toggle.js",
                     "doxygen-awesome-fragment-copy-button.js",
                     "doxygen-awesome-interactive-toc.js")
    )
    init = (
        "<script type=\"text/javascript\">\n"
        "  DoxygenAwesomeDarkModeToggle.init();\n"
        "  DoxygenAwesomeFragmentCopyButton.init();\n"
        "  DoxygenAwesomeInteractiveToc.init();\n"
        "</script>\n"
    )
    banner = '<a class="fastmm-home" href="../../">FastMM documentation</a>\n'
    text = (tmp / "header.html").read_text(encoding="utf-8")
    text = text.replace("</head>", f"{scripts}\n{init}</head>", 1)
    text = re.sub(r"(<body[^>]*>)", r"\1\n" + banner, text, count=1)
    dest.write_text(text, encoding="utf-8")


def _build_doxygen(html: Path) -> None:
    doxygen = os.environ.get("DOXYGEN", "doxygen")
    if shutil.which(doxygen) is None:
        raise RuntimeError(
            f"{doxygen}: not found. Install Doxygen (apt install doxygen) or set "
            "FASTMM_DOCS_CPP_API=0 to build the site without the C++ reference."
        )
    headers = _public_headers()
    out = html.parent
    out.mkdir(parents=True, exist_ok=True)
    header = out / "header.html"
    doxyfile = out / "Doxyfile"
    template = (DOXY_DIR / "Doxyfile.in").read_text(encoding="utf-8")
    for key, value in {
        "@VERSION@": _version(),
        "@OUTPUT@": str(out),
        "@INPUT@": " \\\n                         ".join(str(p) for p in headers),
        "@INCLUDE_DIR@": str(INCLUDE_DIR),
        "@ROOT@": str(ROOT),
        "@PYTHON@": sys.executable,
        "@HEADER@": str(header),
    }.items():
        template = template.replace(key, value)
    doxyfile.write_text(template, encoding="utf-8")
    _write_header(doxygen, doxyfile, header)
    if html.exists():
        shutil.rmtree(html)
    subprocess.run([doxygen, str(doxyfile)], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)
    warnings = out / "doxygen-warnings.log"
    if warnings.is_file() and warnings.stat().st_size:
        n = sum(1 for _ in warnings.open(encoding="utf-8", errors="replace"))
        log.info("doxygen: %d warning line(s) in %s", n, warnings)
    log.info("doxygen: %d public header(s) -> %s", len(headers), html)


def _check_page_links(html: Path) -> list[str]:
    """Every href on docs/api/cpp.md that names a generated page must exist."""
    missing = []
    for href in HREF.findall(CPP_PAGE.read_text(encoding="utf-8")):
        if urlsplit(href).scheme or href.startswith(("#", "/", "..")):
            continue
        if not (html / href.split("#")[0]).exists():
            missing.append(href)
    return missing


def on_post_build(config) -> None:  # noqa: ANN001
    dest = Path(config["site_dir"]) / "api" / "cpp"
    if os.environ.get("FASTMM_DOCS_CPP_API", "1") == "0":
        log.warning("FASTMM_DOCS_CPP_API=0: the site is built without the C++ reference")
        return
    html = CACHE_DIR / "html"
    if _stale(html, _inputs(_public_headers())):
        _build_doxygen(html)
    else:
        log.info("doxygen: %s is up to date", html)
    missing = _check_page_links(html)
    if missing:
        raise RuntimeError(
            "docs/api/cpp.md links to Doxygen pages that do not exist: " + ", ".join(missing)
        )
    dest.mkdir(parents=True, exist_ok=True)
    for src in html.iterdir():
        # docs/api/cpp.md is the landing page at /api/cpp/; Doxygen's own index.html is a stub.
        if src.name == "index.html":
            continue
        target = dest / src.name
        if src.is_dir():
            shutil.copytree(src, target, dirs_exist_ok=True)
        else:
            shutil.copy2(src, target)
