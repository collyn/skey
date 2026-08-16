#!/bin/bash
# publish-all.sh — Publish all package types to gh-pages in ONE commit.
# Avoids race conditions from multiple publish jobs pushing sequentially.
set -e

ORIG_DIR="$(pwd)"
GPG_KEY_EMAIL="${APT_GPG_KEY_EMAIL:-collyn094@gmail.com}"

echo "=== Publishing all packages to gh-pages ==="

# ── Determine workspace ──────────────────────────────────────────────
WORKDIR="$(mktemp -d)"
trap "rm -rf '$WORKDIR'" EXIT

if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    REPO_URL="https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git"
    REPO_NAME="${GITHUB_REPOSITORY##*/}"
    REPO_OWNER="${GITHUB_REPOSITORY_OWNER:-collyn}"
else
    REPO_URL="$(git remote get-url origin)"
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    REPO_NAME="$(basename "$REPO_ROOT")"
    REPO_OWNER="$(echo "$REPO_URL" | sed -n 's|.*[:/]\([^/]*\)/'"$REPO_NAME"'\.git|\1|p')"
    REPO_OWNER="${REPO_OWNER:-collyn}"
fi

GITHUB_PAGES_URL="https://${REPO_OWNER}.github.io/${REPO_NAME}"
FINGERPRINT=$(gpg --fingerprint --with-colons "$GPG_KEY_EMAIL" 2>/dev/null | grep '^fpr:' | head -1 | cut -d: -f10)
FINGERPRINT="${FINGERPRINT:-PLACEHOLDER}"

# ── Clone gh-pages ───────────────────────────────────────────────────
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

cd "$WORKDIR"

# ── Prune old versions (gh-pages storage hygiene) ────────────────────
# Runs BEFORE the copy loops: the release being published is copied in
# next, so keeping 2 here leaves 3 versions on the repo after publish.
# Only files with a version in the name are pruned — repo db files
# (fcitx5-skey.db.tar.gz, *.files*) and repodata/ are left untouched.
prune_keep_newest() {
    local dir="$1" keep="$2" versions file version
    [ -d "$dir" ] || return 0
    versions=$(find "$dir" -maxdepth 1 -type f | \
        sed -nE 's#^.*/fcitx5-skey(-[a-z]+)?[-_]([0-9]+(\.[0-9]+)*).*#\2#p' | \
        sort -Vu | tail -n "$keep")
    [ -n "$versions" ] || return 0
    find "$dir" -maxdepth 1 -type f | while read -r file; do
        version=$(basename "$file" | \
            sed -nE 's/^fcitx5-skey(-[a-z]+)?[-_]([0-9]+(\.[0-9]+)*).*/\2/p')
        [ -n "$version" ] || continue
        if ! grep -qxF "$version" <<<"$versions"; then
            echo "→ Prune: $(basename "$file")"
            rm -f "$file"
        fi
    done
}
prune_keep_newest pool/main/f/fcitx5-skey 2
prune_keep_newest rpm/fedora 2
prune_keep_newest rpm/opensuse 2
prune_keep_newest arch/x86_64 2

# Debug packages are never published to the repos (kept in CI artifacts
# only) — this also sweeps ones published before that rule existed.
find pool/main/f/fcitx5-skey rpm/fedora rpm/opensuse arch/x86_64 \
    -maxdepth 1 -type f \
    \( -name '*debuginfo*' -o -name '*debugsource*' -o -name '*debug-*' \) \
    -delete 2>/dev/null || true

# ── APT: .deb → pool/ + dists/ ───────────────────────────────────────
for deb in "$ORIG_DIR"/*.deb; do
    [ -f "$deb" ] || continue
    echo "→ APT: $(basename "$deb")"
    mkdir -p pool/main/f/fcitx5-skey dists/stable/main/binary-amd64
    cp "$deb" pool/main/f/fcitx5-skey/
done

if ls pool/main/f/fcitx5-skey/*.deb >/dev/null 2>&1; then
    dpkg-scanpackages --arch amd64 pool/ > dists/stable/main/binary-amd64/Packages
    gzip -9c dists/stable/main/binary-amd64/Packages > dists/stable/main/binary-amd64/Packages.gz

    cat > /tmp/apt-release-conf << 'EOF'
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
    apt-ftparchive -c=/tmp/apt-release-conf release dists/stable > dists/stable/Release
    rm -f /tmp/apt-release-conf

    gpg --batch --yes --detach-sign --armor \
        --local-user "$GPG_KEY_EMAIL" \
        -o dists/stable/Release.gpg dists/stable/Release
    gpg --batch --yes --clearsign \
        --local-user "$GPG_KEY_EMAIL" \
        -o dists/stable/InRelease dists/stable/Release
fi

# ── RPM: .rpm → rpm/$distro/ ─────────────────────────────────────────
for rpm in "$ORIG_DIR"/*.rpm; do
    [ -f "$rpm" ] || continue
    case "$rpm" in
        *debuginfo*|*debugsource*) continue ;;  # debug pkgs stay in CI artifacts only
        *fedora*|*.fc[0-9]*|*.fc[0-9][0-9]*) DISTRO="fedora" ;;
        *opensuse*|*.suse*|*tumbleweed*) DISTRO="opensuse" ;;
        *)
            # Fallback: check if filename contains fedora-like release tag (.fc42, .fc41, etc.)
            if echo "$rpm" | grep -qE '\.fc[0-9]+\.'; then
                DISTRO="fedora"
            else
                DISTRO="opensuse"
            fi
            ;;
    esac
    RPM_DIR="rpm/${DISTRO}"
    echo "→ RPM ($DISTRO): $(basename "$rpm")"
    mkdir -p "$RPM_DIR"
    cp "$rpm" "$RPM_DIR/"
    createrepo_c "$RPM_DIR" 2>/dev/null || createrepo "$RPM_DIR" 2>/dev/null || true
    gpg --batch --yes --detach-sign --armor \
        --local-user "$GPG_KEY_EMAIL" \
        -o "$RPM_DIR/repodata/repomd.xml.asc" \
        "$RPM_DIR/repodata/repomd.xml" 2>/dev/null || true
done

# ── Arch: .pkg.tar.zst + .db → arch/x86_64/ ──────────────────────────
ARCH_DIR="arch/x86_64"
mkdir -p "$ARCH_DIR"
for pkg in "$ORIG_DIR"/*.pkg.tar.zst "$ORIG_DIR"/*.pkg.tar.zst.sig \
            "$ORIG_DIR"/*.db.tar.gz "$ORIG_DIR"/*.db.tar.gz.sig \
            "$ORIG_DIR"/*.files.tar.gz "$ORIG_DIR"/*.files.tar.gz.sig; do
    [ -f "$pkg" ] || continue
    case "$pkg" in
        *debug-*) continue ;;  # debug pkgs stay in CI artifacts only
    esac
    cp "$pkg" "$ARCH_DIR/"
    echo "→ Arch: $(basename "$pkg")"
done

# ── Export public keys ────────────────────────────────────────────────
gpg --export --armor "$GPG_KEY_EMAIL" > key.asc 2>/dev/null || true
gpg --export "$GPG_KEY_EMAIL" > key.gpg 2>/dev/null || true

# ── Generate install scripts ──────────────────────────────────────────

# install.sh (APT)
cat > install.sh << 'INSTEOF'
#!/bin/bash
set -e
echo "Adding fcitx5-skey APT repository..."
sudo mkdir -p /etc/apt/keyrings
curl -fsSL GHP_URL/key.asc | sudo gpg --dearmor --yes -o /etc/apt/keyrings/fcitx5-skey.gpg
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/fcitx5-skey.gpg] GHP_URL stable main" | sudo tee /etc/apt/sources.list.d/fcitx5-skey.list > /dev/null
sudo apt update
echo "✓ Run: sudo apt install fcitx5-skey"
INSTEOF
sed -i "s|GHP_URL|${GITHUB_PAGES_URL}|g" install.sh
chmod +x install.sh

# install-fedora.sh
cat > install-fedora.sh << 'INSTEOF'
#!/bin/bash
set -e
echo "Adding fcitx5-skey RPM repository for Fedora..."
sudo rpm --import GHP_URL/key.asc
cat << 'REPO' | sudo tee /etc/yum.repos.d/fcitx5-skey.repo > /dev/null
[fcitx5-skey]
name=fcitx5-skey — Vietnamese SKey input method
baseurl=GHP_URL/rpm/fedora/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=GHP_URL/key.asc
REPO
echo "✓ Run: sudo dnf install fcitx5-skey"
INSTEOF
sed -i "s|GHP_URL|${GITHUB_PAGES_URL}|g" install-fedora.sh
chmod +x install-fedora.sh

# install-opensuse.sh
cat > install-opensuse.sh << 'INSTEOF'
#!/bin/bash
set -e
echo "Adding fcitx5-skey RPM repository for openSUSE..."
sudo rpm --import GHP_URL/key.asc
sudo zypper addrepo --refresh --check --gpgcheck GHP_URL/rpm/opensuse/ fcitx5-skey
echo "✓ Run: sudo zypper install fcitx5-skey"
INSTEOF
sed -i "s|GHP_URL|${GITHUB_PAGES_URL}|g" install-opensuse.sh
chmod +x install-opensuse.sh

# install-arch.sh
cat > install-arch.sh << 'INSTEOF'
#!/bin/bash
set -e
echo "Adding fcitx5-skey Arch Linux repository..."
curl -fsSL GHP_URL/key.asc -o /tmp/fcitx5-skey-key.asc
sudo pacman-key --add /tmp/fcitx5-skey-key.asc
sudo pacman-key --lsign-key GPG_EMAIL
rm -f /tmp/fcitx5-skey-key.asc
if ! grep -q '\[fcitx5-skey\]' /etc/pacman.conf; then
    echo "" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "[fcitx5-skey]" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "Server = GHP_URL/arch/x86_64" | sudo tee -a /etc/pacman.conf > /dev/null
    echo "SigLevel = Optional TrustAll" | sudo tee -a /etc/pacman.conf > /dev/null
fi
sudo pacman -Syu
echo "✓ Run: sudo pacman -S fcitx5-skey"
INSTEOF
sed -i "s|GHP_URL|${GITHUB_PAGES_URL}|g" install-arch.sh
sed -i "s|GPG_EMAIL|${GPG_KEY_EMAIL}|g" install-arch.sh
chmod +x install-arch.sh

# ── Commit and push ──────────────────────────────────────────────────
git checkout gh-pages 2>/dev/null || true
git add -A
git config user.email "github-actions[bot]@users.noreply.github.com"
git config user.name "github-actions[bot]"

if git diff --staged --quiet; then
    echo "No changes to commit."
else
    git commit -m "Publish fcitx5-skey packages

deb + rpm + arch packages for the latest release"
    git push origin gh-pages
    echo "✓ All packages published to gh-pages!"
fi

cd "$ORIG_DIR"
echo ""
echo "=== Done ==="
echo "  APT:     curl -fsSL ${GITHUB_PAGES_URL}/install.sh | sudo bash"
echo "  Fedora:  curl -fsSL ${GITHUB_PAGES_URL}/install-fedora.sh | sudo bash"
echo "  openSUSE: curl -fsSL ${GITHUB_PAGES_URL}/install-opensuse.sh | sudo bash"
echo "  Arch:    curl -fsSL ${GITHUB_PAGES_URL}/install-arch.sh | sudo bash"
