#!/bin/sh
# Entrypoint of the production image (docker/Dockerfile.production). The first argument is a
# config path, a flag for fastmm-live, or another fastmm program:
#   docker run ... fastmm:<version> /etc/fastmm/live.toml --duration 60s
#   docker run ... fastmm:<version> fastmm-top --once
set -eu
case "${1:-}" in
  "")        echo "fastmm: pass a config path, a fastmm-live flag, or a program name" >&2; exit 2 ;;
  fastmm-*)  exec "$@" ;;
  -*)        exec fastmm-live "$@" ;;
  *)         config=$1; shift; exec fastmm-live --config "$config" "$@" ;;
esac
