#!/bin/bash
# ppa-build-upload.sh — Build source package & upload to PPA for given series.
# Usage: ./scripts/ppa-build-upload.sh <version> <series>
set -e

VERSION="$1"
SERIES="$2"

if [ -z "$VERSION" ] || [ -z "$SERIES" ]; then
    echo "Usage: $0 <version> <series>" >&2
    exit 1
fi

ENGINE_VER=$(grep -oP 'set\(SKEY_ENGINE_VERSION "\K[^"]+' CMakeLists.txt)
KEYID=$(gpg --list-secret-keys --with-colons | grep '^sec:' | head -1 | cut -d: -f5)

echo "=== Building fcitx5-skey ${VERSION}-1 for ${SERIES} (engine ${ENGINE_VER}) ==="

# Generate changelog for this series
cat > debian/changelog << CHLOG
fcitx5-skey (${VERSION}-1) ${SERIES}; urgency=medium

  * Release ${VERSION} with skey-engine ${ENGINE_VER}.

 -- Nguyen Tien Huy <collyn094@gmail.com>  $(date -R)
CHLOG

# First series gets -sa (include orig tarball), rest get -sd (skip)
SA_FLAG="-sa"
if [ -f ../fcitx5-skey_${VERSION}.orig.tar.gz ]; then
    echo "→ Orig tarball already exists, using -sd"
    SA_FLAG="-sd"
else
    # Create fresh orig tarball for first series
    echo "→ Creating orig tarball..."
    ./scripts/make-ppa-source.sh "$VERSION"
fi

set -o pipefail
debuild -k"$KEYID" -S ${SA_FLAG} -d 2>&1 | tee /tmp/debuild-${SERIES}.log

# Copy files for dput
cp ../*.dsc ../*.debian.tar.* ../*_source.changes ../*_source.buildinfo . 2>/dev/null || true
if [ "$SA_FLAG" = "-sa" ]; then
    cp ../*.orig.tar.* . 2>/dev/null || true
fi
ls -la *.dsc *.debian.tar.* *_source.changes 2>/dev/null

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
echo "Uploading to ${SERIES}: $CHANGES"
dput ppa-collyn "$CHANGES"
