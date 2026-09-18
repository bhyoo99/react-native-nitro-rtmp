#!/usr/bin/env bash
# Syncs the vendored ireader/media-server libraries (RTMP client/server, FLV muxer,
# MPEG-TS muxer, MP4 reader/writer) and the few ireader/sdk files they need
# (HMAC-SHA256 for the RTMP handshake, URI parsing for rtmp-url.h)
# into cpp/third_party/media-server.
#
# Patches in scripts/patches/media-server-*.patch are applied afterwards (see
# UPSTREAM.md for what they change and why); they are meant to go upstream.
#
# To upgrade: bump the pinned commits below, run this script, review the diff.
# If a patch no longer applies, refresh it against the new upstream files.
#
#   scripts/sync-media-server.sh              # fetch, copy, patch, write UPSTREAM.md
#   scripts/sync-media-server.sh --docs-only  # only rewrite UPSTREAM.md
set -euo pipefail

MEDIA_SERVER_REPO="https://github.com/ireader/media-server.git"
MEDIA_SERVER_COMMIT="5b9d159a7e3a029852407009d388ccd298627dcf"
SDK_REPO="https://github.com/ireader/sdk.git"
SDK_COMMIT="e84a3d1a91cacdd8dedbbc931a2886b99bec4846"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/cpp/third_party/media-server"
PATCHES="$ROOT/scripts/patches"

write_upstream_md() {
cat > "$DEST/UPSTREAM.md" <<MD
# Vendored sources

Do not edit files in this directory by hand. Re-run \`scripts/sync-media-server.sh\`.

| Project | Commit | License |
|---|---|---|
| [ireader/media-server](https://github.com/ireader/media-server) | \`$MEDIA_SERVER_COMMIT\` | MIT (see LICENSE) |
| [ireader/sdk](https://github.com/ireader/sdk) | \`$SDK_COMMIT\` | MIT (see sdk/LICENSE) |

Build flags every consumer (host tests, Android CMake, iOS pod) sets for these sources:
\`NDEBUG\` (upstream asserts fire on server input such as unknown onStatus codes),
\`_IETF_HMAC_\` (digest handshake through the vendored RFC 6234 HMAC), and warnings off.

Patches applied on top of upstream (\`scripts/patches/\`, in order):

| Patch | What it changes | Why |
|---|---|---|
| \`media-server-0001-client-onstatus.patch\` | Adds an optional \`onstatus(level, code, description)\` callback to \`rtmp_client_handler_t\`, called for every \`onStatus\`/\`_error\` before the built-in dispatch; a non-zero return fails \`rtmp_client_input\`. Unknown codes no longer hit \`assert(0)\`/\`printf\`, and an unknown code with level \`error\` now returns -1 instead of 0. | Publishers need the server's status code (\`NetStream.Publish.BadName\`, \`NetConnection.Connect.InvalidApp\`, ...) to report a useful error; upstream returned 0 for \`NetStream.Publish.BadName\`, so a rejected stream key was silent. |

Included from media-server (include/ and source/ only):

| Library | Purpose |
|---|---|
| \`librtmp\` | RTMP client and server state machines (sans-IO), chunking, AMF commands, \`rtmp-url.h\` |
| \`libflv\` | FLV muxer/demuxer, AMF0/AMF3, H.264/H.265/AV1/AAC helpers, Enhanced RTMP |
| \`libmpeg\` | MPEG-TS and MPEG-PS muxer/demuxer (SRT payload) |
| \`libmov\` | MP4 / fragmented MP4 reader and writer (local recording, test fixtures) |

Left out on purpose: \`librtmp/aio\` (needs the sdk event loop), \`librtsp\` (needs ireader/avcodec),
\`librtp\`, \`libhls\`, \`libdash\`, \`libmkv\`, \`libsip\` (not on the publishing path).

Included from sdk: \`sha.h\` plus \`sha.c\`, \`sha1.c\`, \`sha224-256.c\`, \`sha384-512.c\`, \`hmac.c\`
(HMAC-SHA256 for the RTMP handshake), and \`uri-parse\`, \`uri-query\`, \`urlcodec\` (for \`rtmp-url.h\`).
The digest sources are the RFC 6234 reference code, Copyright (c) 2011 IETF Trust,
under the Simplified BSD License stated in each file.
MD
}

if [ "${1:-}" = "--docs-only" ]; then
  write_upstream_md
  echo "Rewrote $DEST/UPSTREAM.md"
  exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fetch_at_commit() { # <repo> <commit> <dir>
  git init -q "$3"
  git -C "$3" remote add origin "$1"
  git -C "$3" fetch -q --depth 1 origin "$2"
  git -C "$3" checkout -q FETCH_HEAD
}

fetch_at_commit "$MEDIA_SERVER_REPO" "$MEDIA_SERVER_COMMIT" "$TMP/media-server"
fetch_at_commit "$SDK_REPO" "$SDK_COMMIT" "$TMP/sdk"

rm -rf "$DEST"
mkdir -p "$DEST/sdk/include" "$DEST/sdk/source"

# Only include/ and source/ of the libraries we use. Tests, Makefiles, IDE
# projects and the aio/ networking layer (needs the full sdk event loop) stay
# out. librtsp (needs ireader/avcodec), librtp and libhls are not on the
# RTMP/SRT publishing path and are left out on purpose.
for lib in librtmp libflv libmpeg libmov; do
  mkdir -p "$DEST/$lib"
  cp -R "$TMP/media-server/$lib/include" "$TMP/media-server/$lib/source" "$DEST/$lib/"
done
cp "$TMP/media-server/LICENSE" "$DEST/LICENSE"
# sha.c is the RFC 6234 "USHA" front end; it dispatches to every SHA variant,
# and hmac.c is what rtmp-handshake.c actually calls. All five are needed to link.
cp "$TMP/sdk/include/sha.h" "$DEST/sdk/include/sha.h"
for f in sha.c sha1.c sha224-256.c sha384-512.c hmac.c; do
  cp "$TMP/sdk/source/digest/$f" "$DEST/sdk/source/$f"
done
# uri-parse / uri-query / urlcodec back rtmp-url.h (rtmp_url_parse: app, stream,
# tcurl, vhost). uri-query.c has no header of its own; it is declared in uri-parse.h.
for f in uri-parse urlcodec; do
  cp "$TMP/sdk/include/$f.h" "$DEST/sdk/include/$f.h"
done
for f in uri-parse uri-query urlcodec; do
  cp "$TMP/sdk/source/$f.c" "$DEST/sdk/source/$f.c"
done
cp "$TMP/sdk/LICENSE" "$DEST/sdk/LICENSE"


for p in "$PATCHES"/media-server-*.patch; do
  [ -e "$p" ] || continue
  echo "Applying $(basename "$p")"
  patch -p1 -d "$DEST" --forward --no-backup-if-mismatch < "$p"
done

write_upstream_md

echo "Synced media-server@${MEDIA_SERVER_COMMIT:0:7} and sdk@${SDK_COMMIT:0:7} into $DEST"
