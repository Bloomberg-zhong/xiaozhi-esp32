# XiaoZhi music server

One HTTP API in front of several **free** music catalogs, so the device only
needs one address: your domain. Python 3.9+; audio and search use the standard
library, while optional Pillow normalizes album artwork.

| Catalog | What it is | Needs |
|---|---|---|
| `archive` | Internet Archive: net labels, live concert recordings, public domain music | nothing |
| `radio` | Live radio stations (Radio Browser), Chinese stations included | nothing |
| `jamendo` | Creative Commons music | free `client_id` |
| `subsonic` | Your own Navidrome / Gonic / Airsonic library | your server |
| `local` | A folder of audio files on this machine | a folder |

Lyrics come from the catalog when it has them, otherwise from the free
[LRCLIB](https://lrclib.net) service. The built-in catalogs do not include
commercial streaming platforms. An empty result means the configured catalogs
did not find the song; it does not establish a copyright restriction. Additional
catalogs can be connected through plugins (below).

### Connect an existing Go Music API gateway

The bundled `gateway_plugin` adapts an operator-configured gateway to this
server's `/search`, `/stream` and `/lyrics` API. Add these settings to your
configuration, keeping your existing providers:

```json
{
  "plugins": ["gateway_plugin"],
  "provider_order": ["local", "gateway", "archive", "radio"],
  "providers": {
    "gateway": {
      "url": "http://host.docker.internal:8080",
      "sources": ["kuwo", "qq", "netease"],
      "max_probes": 8
    }
  }
}
```

`host.docker.internal` reaches the host's existing gateway from Docker Desktop.
Use your gateway's actual address in other deployments. Only that configured
host is trusted for LAN access. Search probes a bounded shortlist, excludes
audio the gateway reports as unavailable, and prioritizes an exact title/artist
match over Demo, Live and other variants. Titles and artists are kept as supplied
by the catalog. Stream requests resolve through the gateway each time; Range
requests and catalog lyrics are forwarded. The adapter does not unlock tracks
that the upstream source refuses to serve.

Album artwork from the gateway's `cover_url`, `cover`, `picUrl`, or album
`picUrl` metadata is fetched through the same SSRF checks as audio. Search
responses expose only this server's `/cover` address.
For a saved track ID after metadata expiry or a server restart, a cover request
can recover the original catalog entry using explicit title/artist lyric tags.
Only a result with the same source and song ID is accepted; missing tags or an
unmatched ID keep the cover unavailable. This lookup runs only for artwork and
does not add catalog requests to audio startup.

## Try it

```bash
python3 scripts/music_server/music_server.py --music-dir ~/Music   # local files + archive + radio
curl "http://127.0.0.1:8090/search?q=piano&limit=3"
```

For covers in a direct Python deployment, install the optional dependency into
that server's virtual environment:

```bash
python3 -m pip install -r scripts/music_server/requirements-artwork.txt
```

The Docker image includes it.
Search and audio remain available without Pillow; cover requests then return
`503`. Local folders support a matching song `.jpg`/`.jpeg`/`.png`/`.webp`, or
`cover`, `folder`, or `front` files with those extensions.

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
| `GET /cover/<id>?size=128` | album cover as baseline RGB JPEG, longest edge at most 128 pixels, at most 32 KiB; `404` without artwork |

A track is `{"id", "title", "artist", "album", "duration_ms", "url", "lyric_url", "cover_url", "source", "live"}`.
`id` is `<catalog>:<catalog id>` and `url` is `/stream/<percent-encoded id>`, so
a client can rebuild the stream address of a saved favorite from the id alone.
Errors are `{"error": "..."}`.
`cover_url` is optional and points to `/cover/<percent-encoded id>?size=128`;
original upstream artwork addresses and Subsonic credentials stay on the server.
The cover route requires the same API key as search and audio. Images retain
their aspect ratio; requests for larger sizes are capped at 128. Input images
are limited to 2 MiB and 16 million pixels.

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

When a LAN proxy uses Fake-IP DNS, a public artwork CDN may resolve to a reserved
address such as `198.18.x.x`, which the default SSRF guard rejects. After checking
the real CDN belongs to your chosen catalog, configure only its exact hostname:

```json
{"trusted_artwork_hosts": ["img1.kuwo.cn", "img2.kuwo.cn", "img3.kuwo.cn", "img4.kuwo.cn"]}
```

The default is an empty list. Entries are hostnames without a scheme, port,
wildcard, or path; subdomains are not included automatically. This exception
applies only to artwork downloads. Audio, search, and lyrics retain their
existing host checks, and artwork redirects to any other untrusted private or
reserved host remain blocked. Keep `allow_private_hosts` disabled when using
this narrow exception. Restart the server after updating its configuration.

## Adding a catalog

Subclass `xzmusic.providers.base.Provider` (`search`, `get`, optionally
`lyrics` and `open_stream`), expose it as `PROVIDERS = {"name": YourClass}` in a
module on `PYTHONPATH`, and list the module in `"plugins"`; configure it under
`"providers": {"name": {...}}`.

## Tests

```bash
python3 -m unittest scripts/tests/test_music_server.py -v   # offline, uses a fake upstream
```
