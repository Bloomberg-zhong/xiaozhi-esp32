# XiaoZhi music server

One HTTP API in front of several **free** music catalogs, so the device only
needs one address: your domain. Standard library only, Python 3.9+.

| Catalog | What it is | Needs |
|---|---|---|
| `archive` | Internet Archive: net labels, live concert recordings, public domain music | nothing |
| `radio` | Live radio stations (Radio Browser), Chinese stations included | nothing |
| `jamendo` | Creative Commons music | free `client_id` |
| `subsonic` | Your own Navidrome / Gonic / Airsonic library | your server |
| `local` | A folder of audio files on this machine | a folder |

Lyrics come from the catalog when it has them, otherwise from the free
[LRCLIB](https://lrclib.net) service. Commercial streaming services are not
included: their catalogs cannot be streamed for free without bypassing their
licensing. Add a catalog you are allowed to use as a plugin (below).

## Try it

```bash
python3 scripts/music_server/music_server.py --music-dir ~/Music   # local files + archive + radio
curl "http://127.0.0.1:8090/search?q=piano&limit=3"
```

## Put it on your domain (HTTPS)

```bash
cd scripts/music_server
cp .env.example .env            # fill in MUSIC_DOMAIN and XZ_MUSIC_API_KEY
cp config.example.json config.json
docker compose up -d
```

Point the domain's DNS at the machine and open ports 80 and 443. Caddy gets the
certificate automatically. A server in mainland China needs an ICP filing for
the domain, otherwise ports 80/443 are blocked.

Then tell the device where it is, either way:

* build time: `CONFIG_MUSIC_SERVER_URL="https://music.example.com"` and
  `CONFIG_MUSIC_SERVER_API_KEY="..."` in the board's `config.json`
  (`sdkconfig_append`), or `idf.py menuconfig` → Xiaozhi Assistant → Music Player;
* at runtime (no reflashing), in the device console:
  `self.music.configure_source(type="http", url="https://music.example.com", api_key="...")`.

## API

All requests except `/health` need `Authorization: Bearer <key>` (or
`X-Api-Key`) when a key is configured.

| Request | Answer |
|---|---|
| `GET /health` | `{"ok": true}` |
| `GET /sources` | catalogs that are enabled |
| `GET /search?q=&limit=&source=` | `{"tracks": [...], "errors": {...}}`; `source` picks one catalog, empty mixes all except live radio; empty `q` gives popular or random songs |
| `GET /track?id=` | one track |
| `GET /stream/<id>` | audio, `Range` supported, always proxied by this server |
| `GET /lyrics/<id>` | LRC text, `404` without lyrics |

A track is `{"id", "title", "artist", "album", "duration_ms", "url", "lyric_url", "source", "live"}`.
`id` is `<catalog>:<catalog id>` and `url` is `/stream/<percent-encoded id>`, so
a client can rebuild the stream address of a saved favorite from the id alone.
Errors are `{"error": "..."}`.

## Safety

* Set an API key before exposing the server. Limits: `rate_limit_per_minute`
  per client and `max_streams` concurrent streams.
* Stream and station addresses come from third parties, so the server refuses
  to connect to private, loopback and link-local addresses (SSRF protection).
  The host of your own Subsonic URL is trusted on purpose.
* Behind a reverse proxy set `"trust_proxy": true` so rate limits use the real
  client address.

## Configuration

`config.json` (see `config.example.json`); environment variables override or
complete it: `XZ_MUSIC_API_KEY`, `JAMENDO_CLIENT_ID`, `MUSIC_DIR`,
`SUBSONIC_URL`/`SUBSONIC_USER`/`SUBSONIC_PASSWORD`. A catalog is used only when
its required settings are present; `"enabled": false` turns one off.

## Adding a catalog

Subclass `xzmusic.providers.base.Provider` (`search`, `get`, optionally
`lyrics` and `open_stream`), expose it as `PROVIDERS = {"name": YourClass}` in a
module on `PYTHONPATH`, and list the module in `"plugins"`; configure it under
`"providers": {"name": {...}}`.

## Tests

```bash
python3 -m unittest scripts/tests/test_music_server.py -v   # offline, uses a fake upstream
```
