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

# Create the orig tarball once. For a retry of an already uploaded upstream
# version, set PPA_REUSE_ORIG=1 so the immutable Launchpad orig is reused.
REUSE_ORIG="${PPA_REUSE_ORIG:-0}"
echo "→ Creating orig tarball..."
PPA_REUSE_ORIG="$REUSE_ORIG" ./scripts/make-ppa-source.sh "$VERSION"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ORIG_TARBALL="${REPO_ROOT}/../fcitx5-skey_${VERSION}.orig.tar.gz"
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

    cat > debian/changelog << CHLOG
fcitx5-skey (${VERSION}-${REVISION}) ${SERIES}; urgency=medium

  * Release ${VERSION} with skey-engine ${ENGINE_VER}.

 -- Nguyen Tien Huy <collyn094@gmail.com>  ${CHLOG_DATE}
CHLOG

    cp debian/changelog "$BUILD_ROOT/fcitx5-skey-${VERSION}/debian/changelog"
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
    dput ppa-collyn "$CHANGES" 2>&1 | tee "$DPUT_LOG"
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
