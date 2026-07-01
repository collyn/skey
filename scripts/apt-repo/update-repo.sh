#!/bin/bash
# update-repo.sh: Add a .deb package to the APT repository on the gh-pages branch.
#
# Usage:
#   ./update-repo.sh <path-to.deb>
#
# Environment variables:
#   APT_GPG_KEY_EMAIL  - GPG key email for signing (default: collyn094@gmail.com)
#   GITHUB_TOKEN        - GitHub token for pushing (auto-set in GitHub Actions)
#
# Works both locally (uses git push) and in GitHub Actions.
set -e

ORIG_DIR="$(pwd)"
DEB_PATH="${1:?Usage: $0 <path-to.deb>}"
GPG_KEY_EMAIL="${APT_GPG_KEY_EMAIL:-collyn094@gmail.com}"

if [[ ! -f "$DEB_PATH" ]]; then
    echo "Error: File not found: $DEB_PATH"
    exit 1
fi

# Resolve absolute path to the .deb
DEB_ABS_PATH="$(realpath "$DEB_PATH")"
DEB_NAME="$(basename "$DEB_ABS_PATH")"
echo "=== Publishing $DEB_NAME to APT repository ==="

# ── Determine workspace ──────────────────────────────────────────────
WORKDIR="$(mktemp -d)"
trap "rm -rf '$WORKDIR'" EXIT

if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    echo "→ CI mode: setting up gh-pages"
    REPO_URL="https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
    REPO_NAME="${GITHUB_REPOSITORY##*/}"
    REPO_OWNER="${GITHUB_REPOSITORY_OWNER:-collyn}"
else
    echo "→ Local mode: setting up gh-pages"
    REPO_URL="$(git remote get-url origin)"
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    REPO_NAME="$(basename "$REPO_ROOT")"
    # Derive owner from remote URL
    REPO_OWNER="$(echo "$REPO_URL" | sed -n 's|.*[:/]\([^/]*\)/'"$REPO_NAME"'\.git|\1|p')"
    REPO_OWNER="${REPO_OWNER:-collyn}"
fi

GITHUB_PAGES_URL="https://${REPO_OWNER}.github.io/${REPO_NAME}"

# Clone gh-pages branch (or create it)
if git ls-remote --exit-code --heads origin gh-pages >/dev/null 2>&1; then
    echo "→ Cloning existing gh-pages branch..."
    git clone --depth=1 -b gh-pages "$REPO_URL" "$WORKDIR"
else
    echo "→ Creating new gh-pages branch..."
    git clone --depth=1 "$REPO_URL" "$WORKDIR"
    cd "$WORKDIR"
    git checkout --orphan gh-pages
    git rm -rf .
    cd "$ORIG_DIR"
fi

# ── Setup directory structure ─────────────────────────────────────────
cd "$WORKDIR"

mkdir -p "pool/main/f/fcitx5-skey"
mkdir -p "dists/stable/main/binary-amd64"

# ── Copy .deb to pool ─────────────────────────────────────────────────
echo "→ Copying $DEB_NAME to pool/main/f/fcitx5-skey/"
cp "$DEB_ABS_PATH" "pool/main/f/fcitx5-skey/$DEB_NAME"

# ── Generate Packages index ───────────────────────────────────────────
echo "→ Generating Packages index..."
dpkg-scanpackages --arch amd64 pool/ > "dists/stable/main/binary-amd64/Packages"
gzip -9c "dists/stable/main/binary-amd64/Packages" > "dists/stable/main/binary-amd64/Packages.gz"

# ── Generate Release file ─────────────────────────────────────────────
echo "→ Generating Release file..."

cat > /tmp/skey-apt-conf-$$ << 'EOF'
APT::FTPArchive::Release {
    Origin "fcitx5-skey";
    Label "fcitx5-skey";
    Suite "stable";
    Codename "stable";
    Architectures "amd64";
    Components "main";
    Description "Vietnamese SKey input method addon for Fcitx5";
};
EOF

apt-ftparchive -c=/tmp/skey-apt-conf-$$ release "dists/stable" > "dists/stable/Release"
rm -f /tmp/skey-apt-conf-$$

# ── Sign Release ──────────────────────────────────────────────────────
echo "→ Signing Release with GPG ($GPG_KEY_EMAIL)..."
if ! gpg --list-secret-keys "$GPG_KEY_EMAIL" >/dev/null 2>&1; then
    echo "Error: GPG key '$GPG_KEY_EMAIL' not found. Import it first:"
    echo "  gpg --import /path/to/private-key.asc"
    exit 1
fi

gpg --yes --detach-sign --armor \
    --local-user "$GPG_KEY_EMAIL" \
    -o "dists/stable/Release.gpg" \
    "dists/stable/Release"

gpg --yes --clearsign \
    --local-user "$GPG_KEY_EMAIL" \
    -o "dists/stable/InRelease" \
    "dists/stable/Release"

# ── Export public key ──────────────────────────────────────────────────
echo "→ Exporting public key..."
gpg --export --armor "$GPG_KEY_EMAIL" > "key.asc"

# ── Generate install.sh ───────────────────────────────────────────────
echo "→ Generating install.sh..."
cat > install.sh << INSTALL_EOF
#!/bin/bash
# Install fcitx5-skey APT repository
# Usage: curl -fsSL ${GITHUB_PAGES_URL}/install.sh | sudo bash
set -e

echo "Adding fcitx5-skey APT repository..."

# Install GPG key
sudo mkdir -p /etc/apt/keyrings
curl -fsSL "${GITHUB_PAGES_URL}/key.asc" | sudo gpg --dearmor --yes -o /etc/apt/keyrings/fcitx5-skey.gpg

# Add apt source
echo "deb [signed-by=/etc/apt/keyrings/fcitx5-skey.gpg] ${GITHUB_PAGES_URL} stable main" \\
    | sudo tee /etc/apt/sources.list.d/fcitx5-skey.list > /dev/null

# Update
sudo apt update

echo ""
echo "✓ fcitx5-skey repository installed!"
echo "  Install with: sudo apt install fcitx5-skey"
INSTALL_EOF
chmod +x install.sh

# ── Commit and push ───────────────────────────────────────────────────
echo "→ Committing changes..."

# Ensure we're on gh-pages (important for new branches)
git checkout gh-pages 2>/dev/null || true

git add .
git config user.email "github-actions[bot]@users.noreply.github.com"
git config user.name "github-actions[bot]"

if git diff --staged --quiet; then
    echo "No changes to commit."
else
    git commit -m "Add ${DEB_NAME} to APT repository

Package: fcitx5-skey
File: pool/main/f/fcitx5-skey/${DEB_NAME}"
    echo "→ Pushing to gh-pages..."
    git push origin gh-pages
    echo "✓ Published to APT repository!"
fi

# ── Done ──────────────────────────────────────────────────────────────
cd "$ORIG_DIR"

echo ""
echo "=== Done! Users can now install with: ==="
echo "  curl -fsSL ${GITHUB_PAGES_URL}/install.sh | sudo bash"
echo "  sudo apt install fcitx5-skey"
