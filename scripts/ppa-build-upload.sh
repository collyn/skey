#!/bin/bash
# ppa-build-upload.sh — Build source package & upload to PPA for all series.
# Usage: ./scripts/ppa-build-upload.sh <version>
set -e

VERSION="$1"
SERIES_LIST="${2:-noble resolute}"  # space-separated, can override

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [series_list]" >&2
    exit 1
fi

ENGINE_VER=$(grep -oP 'set\(SKEY_ENGINE_VERSION "\K[^"]+' CMakeLists.txt)
KEYID=$(gpg --list-secret-keys --with-colons | grep '^sec:' | head -1 | cut -d: -f5)

echo "=== Building fcitx5-skey ${VERSION}-1 for ${SERIES_LIST} (engine ${ENGINE_VER}) ==="

# Generate changelog with MULTIPLE series — Launchpad builds for all of them
cat > debian/changelog << CHLOG
fcitx5-skey (${VERSION}-1) ${SERIES_LIST}; urgency=medium

  * Release ${VERSION} with skey-engine ${ENGINE_VER}.

 -- Nguyen Tien Huy <collyn094@gmail.com>  $(date -R)
CHLOG

# Create fresh orig tarball
echo "→ Creating orig tarball..."
./scripts/make-ppa-source.sh "$VERSION"

set -o pipefail
debuild -k"$KEYID" -S -sa -d 2>&1 | tee /tmp/debuild.log

# Copy files for dput
cp ../*.dsc ../*.orig.tar.* ../*.debian.tar.* ../*_source.changes ../*_source.buildinfo . 2>/dev/null || true
ls -la *.dsc *.debian.tar.* *_source.changes *.orig.tar.* 2>/dev/null

# Configure and run dput
cat > ~/.dput.cf << 'DEOF'
[ppa-collyn]
fqdn = ppa.launchpad.net
method = ftp
incoming = ~collyn094/ubuntu/fcitx5-skey
login = anonymous
allow_unsigned_uploads = 0
DEOF

CHANGES=$(ls fcitx5-skey_*_source.changes 2>/dev/null | head -1)
if [ -z "$CHANGES" ]; then
    echo "Error: No .changes file found" >&2
    ls -la
    exit 1
fi
echo "Uploading: $CHANGES"
dput ppa-collyn "$CHANGES"
