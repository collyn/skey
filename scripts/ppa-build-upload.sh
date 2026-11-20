#!/bin/bash
# ppa-build-upload.sh — Build source package & upload to PPA for all series.
# Usage: ./scripts/ppa-build-upload.sh <version> [series_list]
set -e

VERSION="$1"
SERIES_LIST="${2:-noble resolute}"  # space-separated, can override
DEBIAN_REVISION="${DEBIAN_REVISION:-1}"

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [series_list]" >&2
    exit 1
fi

if ! [[ "$DEBIAN_REVISION" =~ ^[0-9]+$ ]]; then
    echo "Error: DEBIAN_REVISION must be a non-negative integer" >&2
    exit 1
fi

ENGINE_VER=$(grep -oP 'set\(SKEY_ENGINE_VERSION "\K[^"]+' CMakeLists.txt)

# Launchpad only accepts signatures from keys registered to the PPA owner (~collyn094).
# The secret keyring contains multiple keys (incl. the apt-repo key), so pick the
# registered one by fingerprint instead of the first key in the keyring.
# Registered fingerprint on Launchpad: C0172668FE42534745CE48639B355FDD12573852
PPA_KEYID="${PPA_GPG_KEYID:-9B355FDD12573852}"
KEYID=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/{print $10}' | grep -i "${PPA_KEYID}$" | head -1)
if [ -z "$KEYID" ]; then
    echo "Error: PPA signing key ${PPA_KEYID} not found in keyring" >&2
    gpg --list-secret-keys --keyid-format LONG
    exit 1
fi
echo "=== Signing with Launchpad-registered key: ${KEYID} ==="

echo "=== Building fcitx5-skey ${VERSION}-${DEBIAN_REVISION}+ per-series revisions for: ${SERIES_LIST} (engine ${ENGINE_VER}) ==="

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORIG_TARBALL="${REPO_ROOT}/../fcitx5-skey_${VERSION}.orig.tar.gz"
LP_ARCHIVE_API="https://api.launchpad.net/1.0/~collyn094/+archive/ubuntu/fcitx5-skey"

lp_get() {
    curl -fsSL --retry 3 --retry-all-errors --max-time 120 "$@"
}

# Print the URL of this upstream version's orig on the PPA, or nothing if the
# version was never published. Lookup failures return non-zero: building a new
# orig on a guess could conflict with the immutable one on Launchpad.
find_published_orig() {
    local pubs link urls url
    pubs=$(lp_get "${LP_ARCHIVE_API}?ws.op=getPublishedSources&source_name=fcitx5-skey&exact_match=true&order_by_date=true&ws.size=300") || return 1
    link=$(python3 -I -c '
import json, sys
for e in json.loads(sys.stdin.read())["entries"]:
    if e["source_package_version"].startswith(sys.argv[1] + "-"):
        print(e["self_link"])
        break
' "$VERSION" <<< "$pubs") || return 1
    [ -n "$link" ] || return 0
    urls=$(lp_get "${link}?ws.op=sourceFileUrls") || return 1
    url=$(python3 -I -c '
import json, sys
for u in json.loads(sys.stdin.read()):
    if u.endswith("_" + sys.argv[1] + ".orig.tar.gz"):
        print(u)
' "$VERSION" <<< "$urls") || return 1
    [ -n "$url" ] || return 1
    echo "$url"
}

# A reused orig ships the source it was uploaded with. Refuse to upload if HEAD
# has moved on, otherwise a new revision would silently carry old code. Compare
# committed content: uncommitted files never reach a reused orig.
check_orig_matches_head() {
    local tree="$1" head_tree="$2" path drift=0
    mkdir -p "$head_tree"
    git -C "$REPO_ROOT" archive HEAD | tar -x -C "$head_tree"
    while IFS= read -r -d '' path; do
        case "$path" in
            debian/*|.github/*|.claude/*|.agents/*|.aws/*|.codex/*|.gitnexus/*|CLAUDE.md|AGENTS.md) continue ;;
            scripts/ppa-build-upload.sh|scripts/make-ppa-source.sh) continue ;;
        esac
        if ! cmp -s "$head_tree/$path" "$tree/$path"; then
            echo "  differs from orig: $path" >&2
            drift=1
        fi
    done < <(git -C "$REPO_ROOT" ls-tree -r -z --name-only HEAD)
    rm -rf "$head_tree"
    if [ "$drift" != 0 ]; then
        echo "Error: source changed since the ${VERSION} orig was uploaded. Bump the version in CMakeLists.txt." >&2
        return 1
    fi
}

# dput can fail transiently (Launchpad FTP answers 550 at random). Retries need
# -f because dput otherwise refuses a .changes it has already logged.
upload_changes() {
    local changes="$1" log="$2" force="" attempt=1
    local attempts="${PPA_DPUT_ATTEMPTS:-5}" delay="${PPA_DPUT_RETRY_DELAY:-30}"
    while ! dput $force ppa-collyn "$changes" 2>&1 | tee "$log"; do
        if [ "$attempt" -ge "$attempts" ]; then
            return 1
        fi
        echo "dput attempt ${attempt}/${attempts} failed; retrying in ${delay}s..."
        sleep "$delay"
        delay=$((delay * 2))
        attempt=$((attempt + 1))
        force="-f"
    done
}

# Launchpad treats an orig filename as immutable, so an upstream version that is
# already on the PPA must reuse those exact bytes. auto (default) decides from
# the PPA; 1 reuses the local file; 0 always builds a new orig.
REUSE_ORIG="${PPA_REUSE_ORIG:-auto}"
if [ "$REUSE_ORIG" = auto ]; then
    PUBLISHED_ORIG=$(find_published_orig) || {
        echo "Error: could not look up ${VERSION} on the PPA" >&2
        exit 1
    }
    if [ -n "$PUBLISHED_ORIG" ]; then
        echo "→ ${VERSION} is already on the PPA; downloading its orig: ${PUBLISHED_ORIG}"
        lp_get --max-time 900 -o "${ORIG_TARBALL}.tmp" "$PUBLISHED_ORIG" || {
            echo "Error: could not download the published orig. Bump the version in CMakeLists.txt." >&2
            rm -f "${ORIG_TARBALL}.tmp"
            exit 1
        }
        mv -f "${ORIG_TARBALL}.tmp" "$ORIG_TARBALL"
        REUSE_ORIG=1
    else
        REUSE_ORIG=0
    fi
fi
echo "→ Preparing orig tarball (reuse=${REUSE_ORIG})..."
PPA_REUSE_ORIG="$REUSE_ORIG" ./scripts/make-ppa-source.sh "$VERSION"

if [ ! -f "$ORIG_TARBALL" ]; then
    echo "Error: missing orig tarball: $ORIG_TARBALL" >&2
    exit 1
fi

# Configure dput once
cat > ~/.dput.cf << 'DEOF'
[ppa-collyn]
fqdn = ppa.launchpad.net
method = ftp
incoming = ~collyn094/ubuntu/fcitx5-skey
login = anonymous
passive_ftp = 1
allow_unsigned_uploads = 0
DEOF

set -o pipefail
# Launchpad accepts ONE series per .changes — build and upload each series separately.
# Honor SOURCE_DATE_EPOCH (set by the workflow from the HEAD commit time) so the
# generated changelog — and thus the .debian.tar.xz — is reproducible.
CHLOG_DATE=$(date -R ${SOURCE_DATE_EPOCH:+-d @"$SOURCE_DATE_EPOCH"})

BUILD_ROOT="$(mktemp -d)"
cleanup_build_root() { rm -rf "$BUILD_ROOT"; }
trap cleanup_build_root EXIT
cp -f "$ORIG_TARBALL" "$BUILD_ROOT/"
ORIG_BASENAME="$(basename "$ORIG_TARBALL")"

if [ "$REUSE_ORIG" = 1 ]; then
    tar -xzf "$BUILD_ROOT/$ORIG_BASENAME" -C "$BUILD_ROOT"
    check_orig_matches_head "$BUILD_ROOT/fcitx5-skey-${VERSION}" "$BUILD_ROOT/head-tree"
fi

# Do not let artifacts from an interrupted or previous run be selected by the
# glob below.  debuild writes all source artifacts one directory above the
# checkout, and the cleanup at the end of each iteration cannot cover a run
# that stopped before reaching it.
rm -f "$BUILD_ROOT"/fcitx5-skey_*_source.changes "$BUILD_ROOT"/fcitx5-skey_*.dsc \
      "$BUILD_ROOT"/fcitx5-skey_*.debian.tar.xz "$BUILD_ROOT"/fcitx5-skey_*.buildinfo

if [ "$REUSE_ORIG" = 1 ]; then
    # The upstream orig is already in Launchpad. Sending it again makes dput
    # open a second FTP transfer and is unnecessary for a new Debian revision.
    SA_FLAG="-sd"
else
    SA_FLAG="-sa"
fi
SERIES_INDEX=0
for SERIES in ${SERIES_LIST}; do
    # Launchpad's incoming queue is global: the same .changes/.dsc filenames
    # cannot be uploaded twice for different series. Give each upload a
    # distinct Debian revision while keeping the upstream version unchanged.
    REVISION=$((DEBIAN_REVISION + SERIES_INDEX))
    echo "=== Series: ${SERIES} (${SA_FLAG}) ==="

    # Build only from the clean orig tree. The checkout may be on NixOS and
    # contain read-only mounts such as .cache/nixos/nix/store; dh_clean must
    # never see those paths.
    rm -rf "$BUILD_ROOT/fcitx5-skey-${VERSION}"
    tar -xzf "$BUILD_ROOT/$ORIG_BASENAME" -C "$BUILD_ROOT"
    cp -a "$REPO_ROOT/debian" "$BUILD_ROOT/fcitx5-skey-${VERSION}/"

    cat > "$BUILD_ROOT/fcitx5-skey-${VERSION}/debian/changelog" << CHLOG
fcitx5-skey (${VERSION}-${REVISION}) ${SERIES}; urgency=medium

  * Release ${VERSION} with skey-engine ${ENGINE_VER}.

 -- Nguyen Tien Huy <collyn094@gmail.com>  ${CHLOG_DATE}
CHLOG

    (
        cd "$BUILD_ROOT/fcitx5-skey-${VERSION}"
        debuild -k"$KEYID" -S ${SA_FLAG} -d 2>&1 | tee /tmp/debuild-${SERIES}.log
    )

    # debuild writes the artifacts to the parent dir. Never copy them into the
    # source tree: the next iteration's dpkg-source would trip over the binary
    # .orig.tar.gz/.debian.tar.xz ("unrepresentable changes to source").
    CHANGES=$(ls "$BUILD_ROOT"/fcitx5-skey_*_source.changes 2>/dev/null | head -1)
    if [ -z "$CHANGES" ]; then
        echo "Error: No .changes file found for ${SERIES}" >&2
        ls -la "$BUILD_ROOT"
        exit 1
    fi

    # Guard: the .changes must target exactly this series, otherwise Launchpad rejects it.
    DIST=$(grep '^Distribution:' "$CHANGES" | awk '{print $2}')
    if [ "$DIST" != "$SERIES" ]; then
        echo "Error: .changes Distribution '$DIST' does not match series '$SERIES'" >&2
        exit 1
    fi

    echo "Uploading to ${SERIES}: $CHANGES"
    DPUT_LOG="/tmp/dput-${SERIES}.log"
    upload_changes "$CHANGES" "$DPUT_LOG" || {
        echo "Error: dput failed for ${SERIES} after all attempts." >&2
        exit 1
    }
    if grep -q "Nothing more to do" "$DPUT_LOG"; then
        echo "Error: dput skipped ${SERIES}; the upload filenames already exist. Choose a higher Debian revision." >&2
        exit 1
    fi
    echo "→ Done: ${SERIES}"

    # The first upload of a new upstream version carries the orig tarball;
    # subsequent series reuse the same immutable orig from Launchpad.
    SA_FLAG="-sd"

    # Clean up artifacts from this iteration so the next series' glob
    # doesn't pick up stale .changes / .dsc from the previous series.
    rm -f "$BUILD_ROOT"/fcitx5-skey_*_source.changes "$BUILD_ROOT"/fcitx5-skey_*.dsc \
          "$BUILD_ROOT"/fcitx5-skey_*.debian.tar.xz "$BUILD_ROOT"/fcitx5-skey_*.buildinfo
    SERIES_INDEX=$((SERIES_INDEX + 1))
done

echo "=== All series uploaded ==="
