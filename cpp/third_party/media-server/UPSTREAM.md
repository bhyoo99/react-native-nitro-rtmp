# Vendored sources

Do not edit files in this directory by hand. Re-run `scripts/sync-media-server.sh`.

| Project | Commit | License |
|---|---|---|
| [ireader/media-server](https://github.com/ireader/media-server) | `5b9d159a7e3a029852407009d388ccd298627dcf` | MIT (see LICENSE) |
| [ireader/sdk](https://github.com/ireader/sdk) | `e84a3d1a91cacdd8dedbbc931a2886b99bec4846` | MIT (see sdk/LICENSE) |

Build flags every consumer (host tests, Android CMake, iOS pod) sets for these sources:
`NDEBUG` (upstream asserts fire on server input such as unknown onStatus codes),
`_IETF_HMAC_` (digest handshake through the vendored RFC 6234 HMAC), and warnings off.

Patches applied on top of upstream (`scripts/patches/`, in order):

| Patch | What it changes | Why |
|---|---|---|
| `media-server-0001-client-onstatus.patch` | Adds an optional `onstatus(level, code, description)` callback to `rtmp_client_handler_t`, called for every `onStatus`/`_error` before the built-in dispatch; a non-zero return fails `rtmp_client_input`. Unknown codes no longer hit `assert(0)`/`printf`, and an unknown code with level `error` now returns -1 instead of 0. | Publishers need the server's status code (`NetStream.Publish.BadName`, `NetConnection.Connect.InvalidApp`, ...) to report a useful error; upstream returned 0 for `NetStream.Publish.BadName`, so a rejected stream key was silent. |

Included from media-server (include/ and source/ only):

| Library | Purpose |
|---|---|
| `librtmp` | RTMP client and server state machines (sans-IO), chunking, AMF commands, `rtmp-url.h` |
| `libflv` | FLV muxer/demuxer, AMF0/AMF3, H.264/H.265/AV1/AAC helpers, Enhanced RTMP |
| `libmpeg` | MPEG-TS and MPEG-PS muxer/demuxer (SRT payload) |
| `libmov` | MP4 / fragmented MP4 reader and writer (local recording, test fixtures) |

Left out on purpose: `librtmp/aio` (needs the sdk event loop), `librtsp` (needs ireader/avcodec),
`librtp`, `libhls`, `libdash`, `libmkv`, `libsip` (not on the publishing path).

Included from sdk: `sha.h` plus `sha.c`, `sha1.c`, `sha224-256.c`, `sha384-512.c`, `hmac.c`
(HMAC-SHA256 for the RTMP handshake), and `uri-parse`, `uri-query`, `urlcodec` (for `rtmp-url.h`).
The digest sources are the RFC 6234 reference code, Copyright (c) 2011 IETF Trust,
under the Simplified BSD License stated in each file.
