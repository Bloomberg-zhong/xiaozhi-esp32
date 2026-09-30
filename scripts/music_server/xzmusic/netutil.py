"""Outbound HTTP with SSRF protection, size limits and a small TTL cache.

Station and catalog data come from third parties, so their URLs are not
trusted: hosts that resolve to private, loopback or link-local addresses are
refused unless the operator trusted them explicitly (for example the Navidrome
server on the LAN) or turned on `allow_private_hosts`.
"""

import ipaddress
import json
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import OrderedDict
from typing import Dict, Iterable, Optional

from .models import ProviderError

USER_AGENT = "xiaozhi-music-server/2.0 (+https://github.com/78/xiaozhi-esp32)"
MAX_JSON_BYTES = 4 * 1024 * 1024


class BlockedHost(ProviderError):
    pass


def _is_public(address: str) -> bool:
    ip = ipaddress.ip_address(address)
    return not (
        ip.is_private
        or ip.is_loopback
        or ip.is_link_local
        or ip.is_multicast
        or ip.is_reserved
        or ip.is_unspecified
    )


class _GuardedRedirects(urllib.request.HTTPRedirectHandler):
    max_redirections = 4

    def __init__(self, fetcher: "Fetcher"):
        self._fetcher = fetcher

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        self._fetcher.check_url(urllib.parse.urljoin(req.full_url, newurl))
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Fetcher:
    def __init__(
        self,
        allow_private_hosts: bool = False,
        trusted_hosts: Iterable[str] = (),
        timeout: float = 15.0,
    ):
        self.allow_private_hosts = allow_private_hosts
        self.trusted_hosts = {h.lower() for h in trusted_hosts}
        self.timeout = timeout
        self._opener = urllib.request.build_opener(_GuardedRedirects(self))

    def trust(self, url: str) -> None:
        host = urllib.parse.urlsplit(url).hostname
        if host:
            self.trusted_hosts.add(host.lower())

    def check_url(self, url: str) -> None:
        parts = urllib.parse.urlsplit(url)
        if parts.scheme not in ("http", "https") or not parts.hostname:
            raise BlockedHost("unsupported URL")
        host = parts.hostname.lower()
        if self.allow_private_hosts or host in self.trusted_hosts:
            return
        try:
            infos = socket.getaddrinfo(host, parts.port or 443, proto=socket.IPPROTO_TCP)
        except socket.gaierror as error:
            raise ProviderError("cannot resolve %s" % host) from error
        for info in infos:
            if not _is_public(info[4][0]):
                raise BlockedHost("refusing to connect to a private address")

    def open(self, url: str, headers: Optional[Dict[str, str]] = None, timeout=None):
        """Returns the response object; the caller must close it."""
        self.check_url(url)
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
        try:
            return self._opener.open(request, timeout=timeout or self.timeout)
        except urllib.error.HTTPError as error:
            if error.code == 416:  # A Range past the end is an answer, not a failure
                return error
            raise ProviderError("upstream returned HTTP %d" % error.code) from error
        except (urllib.error.URLError, OSError, ValueError) as error:
            raise ProviderError("upstream request failed: %s" % error) from error

    def get_bytes(self, url: str, headers=None, max_bytes: int = MAX_JSON_BYTES) -> bytes:
        response = self.open(url, headers)
        try:
            data = response.read(max_bytes + 1)
        except OSError as error:
            raise ProviderError("upstream read failed: %s" % error) from error
        finally:
            response.close()
        if len(data) > max_bytes:
            raise ProviderError("upstream response too large")
        return data

    def get_json(self, url: str, params=None, headers=None):
        if params:
            query = urllib.parse.urlencode(params, doseq=True)
            url += ("&" if "?" in url else "?") + query
        try:
            return json.loads(self.get_bytes(url, headers).decode("utf-8"))
        except ValueError as error:
            raise ProviderError("upstream returned invalid JSON") from error


class TTLCache:
    """Small thread-safe LRU cache with per-entry expiry."""

    def __init__(self, max_entries: int = 512):
        self._max = max_entries
        self._data: "OrderedDict[object, tuple]" = OrderedDict()
        self._lock = threading.Lock()

    def get(self, key, default=None):
        with self._lock:
            item = self._data.get(key)
            if item is None:
                return default
            expires, value = item
            if expires < time.monotonic():
                del self._data[key]
                return default
            self._data.move_to_end(key)
            return value

    def set(self, key, value, ttl: float) -> None:
        with self._lock:
            self._data[key] = (time.monotonic() + ttl, value)
            self._data.move_to_end(key)
            while len(self._data) > self._max:
                self._data.popitem(last=False)


class RateLimiter:
    """Token bucket per client address."""

    def __init__(self, per_minute: int):
        self.per_minute = per_minute
        self._buckets: Dict[str, list] = {}
        self._lock = threading.Lock()

    def allow(self, client: str) -> bool:
        if self.per_minute <= 0:
            return True
        now = time.monotonic()
        with self._lock:
            tokens, last = self._buckets.get(client, (float(self.per_minute), now))
            tokens = min(float(self.per_minute), tokens + (now - last) * self.per_minute / 60.0)
            allowed = tokens >= 1.0
            if allowed:
                tokens -= 1.0
            self._buckets[client] = [tokens, now]
            if len(self._buckets) > 10000:  # Forget idle clients
                for key in list(self._buckets)[:5000]:
                    del self._buckets[key]
            return allowed
