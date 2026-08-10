#!/bin/bash
# make-ppa-source.sh — Generate .orig.tar.gz for Launchpad PPA upload.
#
# Bundles fcitx5-skey source + skey-engine (Rust) into a single orig tarball
# that Launchpad buildd can build without internet access.
#
# Usage:
#   ./scripts/make-ppa-source.sh [version]
#
# If version is omitted, it is extracted from CMakeLists.txt.
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

# ── Resolve version ──────────────────────────────────────────────────────
if [ -n "$1" ]; then
    VERSION="$1"
else
    VERSION=$(grep -oP 'project\(fcitx5-skey VERSION \K[0-9.]+' CMakeLists.txt)
fi

if [ -z "$VERSION" ]; then
    echo "Error: could not determine version from CMakeLists.txt" >&2
    exit 1
fi

ENGINE_VERSION=$(grep -oP 'set\(SKEY_ENGINE_VERSION "\K[^"]+' CMakeLists.txt)
PKG_NAME="fcitx5-skey"
TARBALL="${PKG_NAME}_${VERSION}.orig.tar.gz"
WORKDIR="$(mktemp -d)"
STAGING="${WORKDIR}/${PKG_NAME}-${VERSION}"

echo "=== Creating orig tarball for ${PKG_NAME} v${VERSION} ==="
echo "  Engine version: ${ENGINE_VERSION}"
echo "  Output: ../${TARBALL}"

# ── Cleanup trap ─────────────────────────────────────────────────────────
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

# ── Copy project source (exclude build artifacts and CI) ─────────────────
mkdir -p "$STAGING"

echo "→ Copying project source..."
rsync -a \
    --exclude='.git' \
    --exclude='.venv/' \
    --exclude='.env/' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='build/' \
    --exclude='build-deb/' \
    --exclude='build-ppa/' \
    --exclude='*.deb' \
    --exclude='*.rpm' \
    --exclude='*.pkg.tar.zst' \
    --exclude='*.sig' \
    --exclude='.github/' \
    --exclude='.claude/' \
    --exclude='CLAUDE.md' \
    --exclude='AGENTS.md' \
    --exclude='node_modules/' \
    ./ "$STAGING/"

# ── Clone skey-engine into the staging tree ──────────────────────────────
echo "→ Cloning skey-engine ${ENGINE_VERSION}..."
git clone --branch "${ENGINE_VERSION}" --depth 1 \
    https://github.com/collyn/skey-engine.git \
    "${STAGING}/skey-engine"

# Remove engine's .git so it doesn't confuse dpkg-source
rm -rf "${STAGING}/skey-engine/.git"

# ── Create the orig tarball ──────────────────────────────────────────────
echo "→ Creating ${TARBALL}..."
tar czf "${REPO_ROOT}/../${TARBALL}" -C "$WORKDIR" "${PKG_NAME}-${VERSION}"

echo ""
echo "=== Done ==="
echo "  Orig tarball: $(readlink -f "${REPO_ROOT}/../${TARBALL}")"
echo ""
echo "Next steps:"
echo "  1. cd $(readlink -f "${REPO_ROOT}")"
echo "  2. debuild -S -sa"
echo "  3. lintian --pedantic ../${PKG_NAME}_${VERSION}-*_source.changes"
echo "  4. dput ppa:collyn094/${PKG_NAME} ../${PKG_NAME}_${VERSION}-*_source.changes"
