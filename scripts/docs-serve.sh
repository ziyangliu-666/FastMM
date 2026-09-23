#!/usr/bin/env bash
# Builds or serves the documentation site (mkdocs.yml) in its own virtual environment.
#
#   ./scripts/docs-serve.sh                 serve on http://127.0.0.1:8000, reload on edit
#   ./scripts/docs-serve.sh --api           the same, with the generated C++ and Python reference
#   ./scripts/docs-serve.sh --build         one strict build into site/, as CI does
#   ./scripts/docs-serve.sh --port 8080     another port
#
# The virtual environment is build/docs-venv, created from docs/requirements.txt. Nothing is
# installed outside it: no conda environment and no system site-packages are touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VENV="$ROOT/build/docs-venv"
REQ="$ROOT/docs/requirements.txt"
PORT=8000
MODE=serve
CPP_API=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) MODE=build; CPP_API=1; shift ;;
    --api) CPP_API=1; shift ;;
    --port) PORT="$2"; shift 2 ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "$0: unknown option $1" >&2; exit 2 ;;
  esac
done

# A system python, never the one an activated conda environment provides.
python_bin() {
  for p in /usr/bin/python3 "$(command -v python3.13 || true)" "$(command -v python3.12 || true)" \
           "$(command -v python3.11 || true)" "$(command -v python3 || true)"; do
    [[ -x "$p" ]] || continue
    "$p" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null &&
      { echo "$p"; return 0; }
  done
  echo "docs-serve: no python 3.10 or newer found" >&2
  return 1
}

if [[ ! -x "$VENV/bin/mkdocs" || "$REQ" -nt "$VENV/bin/mkdocs" ]]; then
  PY="$(python_bin)"
  echo "docs-serve: installing docs/requirements.txt into $VENV ($PY)"
  "$PY" -m venv --clear "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet --requirement "$REQ"
  touch "$VENV/bin/mkdocs"
fi

if [[ "$CPP_API" == 1 ]] && ! command -v doxygen >/dev/null; then
  echo "docs-serve: doxygen is not installed; the C++ reference will be missing" >&2
  echo "docs-serve: install it with 'sudo apt install doxygen', or drop --api" >&2
  exit 1
fi

export FASTMM_DOCS_CPP_API="$CPP_API"

if [[ "$MODE" == build ]]; then
  exec "$VENV/bin/mkdocs" build --strict
fi

echo "docs-serve: http://127.0.0.1:$PORT  (C++ reference: $([[ $CPP_API == 1 ]] && echo on || echo 'off, pass --api'))"
exec "$VENV/bin/mkdocs" serve --dev-addr "127.0.0.1:$PORT"
