# MeshOS repeater web assets

The MeshOS repeater firmware (ESP32 boards with WiFi) serves a built-in web
interface. These are the asset files it embeds, kept here as source so the build
can embed them back in and the firmware's data region reproduces byte-for-byte.

Extracted from `MeshOS-Repeater-HeltecV4-v1.6.1.bin` (MeshOS repeater 1.6.1,
built on MeshCore v1.17.1 / commit d929643).

| File | In the binary | Served as |
|------|---------------|-----------|
| `index.html` | plain, NUL-terminated C string at DROM offset `0x43e6` (95,160 bytes) | the dashboard at `/`, verbatim |
| `update.html.gz` | gzip blob at DROM offset `0x214f3` (53,715 bytes, inflates to 169,232) | the OTA page at `/update`, with `Content-Encoding: gzip` |
| `update.html` | — | the inflated `update.html.gz`, for reading only; not embedded |

`index.html` is the MeshOS dashboard: a single-page app with the overview,
channels, network, packets, settings, update, admin and console tabs. The MeshOS
logo is an inline base64 PNG inside it, so there is no separate image file.
`logo.png` (240x160) is the MeshOS logo. It ships **inline** as a base64 PNG
inside `index.html`, so it is embedded as part of that file, not separately; it
is kept here only so the image is viewable on its own.

`update.html` is the stock [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA)
page and is unmodified.

## Re-embedding

`tools/embed_web.py` turns these into `examples/meshos_repeater/web_assets.h`:

```sh
python3 tools/embed_web.py examples/meshos_repeater/web examples/meshos_repeater/web_assets.h
```

It emits `INDEX_HTML` / `INDEX_HTML_len` and `UPDATE_HTML_GZ` / `UPDATE_HTML_GZ_len`
as byte arrays holding the exact bytes above, so the embedded copy matches the
shipped firmware.

The gzip blob is kept as-is rather than recompressed: its header has `mtime=0`
and max-compression flags, but recompressing depends on the exact zlib build, so
the stored `.gz` is the source of truth for a byte-identical result.
