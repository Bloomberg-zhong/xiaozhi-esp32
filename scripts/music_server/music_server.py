#!/usr/bin/env python3
"""XiaoZhi music server.

One HTTP API in front of several free music catalogs (Internet Archive,
Jamendo, live radio, your own Navidrome or a folder of files), so the device
only needs a single address: your domain.

    python3 scripts/music_server/music_server.py --config config.json
    python3 scripts/music_server/music_server.py --music-dir ~/Music      # quick start

Audio/search need only the Python standard library; album covers optionally use
Pillow. See README.md for HTTPS deployment and docs/music-player.md for the API.
"""

import argparse
import logging
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from xzmusic.httpapi import serve  # noqa: E402
from xzmusic.service import MusicService, load_config  # noqa: E402


def main(argv=None):
    parser = argparse.ArgumentParser(description="XiaoZhi music server")
    parser.add_argument("--config", help="JSON config file (see config.example.json)")
    parser.add_argument("--music-dir", help="serve a folder of music files (local provider)")
    parser.add_argument("--host", help="listen address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, help="listen port (default 8090)")
    parser.add_argument("--api-key", help="require this key (or set XZ_MUSIC_API_KEY)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    config = load_config(args.config)
    if args.music_dir:
        config["providers"].setdefault("local", {})["music_dir"] = args.music_dir
    if args.api_key:
        config.setdefault("api_keys", []).append(args.api_key)

    service = MusicService(config)
    if not service.providers:
        parser.error("no music source is available: enable a provider in the config")
    host = args.host or config.get("host", "0.0.0.0")
    port = args.port or int(config.get("port", 8090))
    server = serve(service, host, port, bool(config.get("trust_proxy", False)))

    print("Music server on http://%s:%d" % (host, port))
    for source in service.sources():
        print("  - %s: %s%s" % (source["name"], source["description"], " (live)" if source["live"] else ""))
    if not service.api_keys:
        print("WARNING: no API key is set; anyone who can reach this port can use the server.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
