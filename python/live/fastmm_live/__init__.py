"""FastMM live runtime: network stack, venue connectors and OpenSSL for fastmm."""

from __future__ import annotations

import os

import certifi

import fastmm

from . import _live

__version__: str = _live.__version__

if fastmm.__version__ != __version__:
    raise ImportError(
        f"fastmm_live {__version__} needs fastmm {__version__}, found fastmm {fastmm.__version__}"
    )

# Last CA bundle before OpenSSL's compiled-in paths: after SSL_CERT_FILE, SSL_CERT_DIR and the
# system bundles.
_live.set_ca_fallback_file(certifi.where())

# A child forked while a session runs cannot run a session or publish parameters.
os.register_at_fork(after_in_child=_live._after_fork_in_child)

build_info = _live.build_info
ca_locations = _live.ca_locations
self_test = _live.self_test

__all__ = ["__version__", "build_info", "ca_locations", "self_test"]
