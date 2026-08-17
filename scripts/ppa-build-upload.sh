#!/bin/bash
# ppa-build-upload.sh — Build source package & upload to PPA for all series.
# Usage: ./scripts/ppa-build-upload.sh <version> [series_list]
set -e

VERSION="$1"
SERIES_LIST="${2:-noble resolute}"  # space-separated, can override

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [series_list]" >&2
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

echo "=== Building fcitx5-skey ${VERSION}-1 for: ${SERIES_LIST} (engine ${ENGINE_VER}) ==="

# Create fresh orig tarball once; first series uploads it (-sa), the rest skip (-sd).
echo "→ Creating orig tarball..."
./scripts/make-ppa-source.sh "$VERSION"

# Configure dput once
cat > ~/.dput.cf << 'DEOF'
[ppa-collyn]
fqdn = ppa.launchpad.net
method = ftp
incoming = ~collyn094/ubuntu/fcitx5-skey
login = anonymous
allow_unsigned_uploads = 0
DEOF

set -o pipefail
# Launchpad accepts ONE series per .changes — build and upload each series separately.
SA_FLAG="-sa"
for SERIES in ${SERIES_LIST}; do
    echo "=== Series: ${SERIES} (${SA_FLAG}) ==="

    cat > debian/changelog << CHLOG
fcitx5-skey (${VERSION}-1) ${SERIES}; urgency=medium

  * Release ${VERSION} with skey-engine ${ENGINE_VER}.

 -- Nguyen Tien Huy <collyn094@gmail.com>  $(date -R)
CHLOG

    debuild -k"$KEYID" -S ${SA_FLAG} -d 2>&1 | tee /tmp/debuild-${SERIES}.log

    # Copy files for dput
    cp ../*.dsc ../*.orig.tar.* ../*.debian.tar.* ../*_source.changes ../*_source.buildinfo . 2>/dev/null || true
    ls -la *.dsc *.debian.tar.* *_source.changes *.orig.tar.* 2>/dev/null

    CHANGES=$(ls fcitx5-skey_*_source.changes 2>/dev/null | head -1)
    if [ -z "$CHANGES" ]; then
        echo "Error: No .changes file found for ${SERIES}" >&2
        ls -la
        exit 1
    fi

    # Guard: the .changes must target exactly this series, otherwise Launchpad rejects it.
    DIST=$(grep '^Distribution:' "$CHANGES" | awk '{print $2}')
    if [ "$DIST" != "$SERIES" ]; then
        echo "Error: .changes Distribution '$DIST' does not match series '$SERIES'" >&2
        exit 1
    fi

    echo "Uploading to ${SERIES}: $CHANGES"
    dput ppa-collyn "$CHANGES"
    echo "→ Done: ${SERIES}"
    SA_FLAG="-sd"
done

echo "=== All series uploaded ==="
