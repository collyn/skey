#!/bin/bash
# generate-key.sh: Generate GPG keypair for signing the APT repository.
# Run ONCE locally, then add the private key to GitHub Secrets.
# The public key will be hosted on gh-pages for users to install.
set -e

KEY_NAME="${1:-fcitx5-skey apt repo}"
KEY_EMAIL="${2:-fcitx5-skey@collyn.github.io}"
KEY_COMMENT="APT repository signing key for fcitx5-skey"
OUTPUT_DIR="${3:-.}"

echo "=== Generating GPG key for APT repository ==="
echo ""

# Check if key already exists
if gpg --list-secret-keys "$KEY_EMAIL" 2>/dev/null; then
    echo "Key already exists for $KEY_EMAIL"
    echo "Delete it first with: gpg --delete-secret-key '$KEY_EMAIL' && gpg --delete-key '$KEY_EMAIL'"
    echo "Or choose a different email."
    exit 1
fi

# Generate GPG key without passphrase (required for CI automation)
cat > /tmp/skey-gpg-batch << EOF
%echo Generating GPG key...
Key-Type: RSA
Key-Length: 4096
Name-Real: $KEY_NAME
Name-Comment: $KEY_COMMENT
Name-Email: $KEY_EMAIL
Expire-Date: 0
%no-protection
%commit
%echo Done
EOF

gpg --batch --generate-key /tmp/skey-gpg-batch
rm -f /tmp/skey-gpg-batch

echo ""
echo "=== Exporting keys ==="

# Export public key (for users to install)
gpg --export --armor "$KEY_EMAIL" > "$OUTPUT_DIR/key.asc"
echo "→ Public key saved to: $OUTPUT_DIR/key.asc"

# Export private key (for GitHub Secrets)
gpg --export-secret-keys --armor "$KEY_EMAIL" > "$OUTPUT_DIR/private-key.asc"
echo "→ Private key saved to: $OUTPUT_DIR/private-key.asc"

# Get key fingerprint
FINGERPRINT=$(gpg --list-keys --with-colons "$KEY_EMAIL" | grep '^fpr:' | head -1 | cut -d: -f10)
echo ""
echo "=== GPG Key Details ==="
echo "Fingerprint: $FINGERPRINT"
echo "Email:       $KEY_EMAIL"

echo ""
echo "=== Next Steps ==="
echo ""
echo "1. Add the private key to GitHub Secrets:"
echo "   gh secret set APT_GPG_PRIVATE_KEY --body \"\$(cat $OUTPUT_DIR/private-key.asc)\""
echo ""
echo "   Or manually at:"
echo "   https://github.com/collyn/skey/settings/secrets/actions"
echo "   Name:  APT_GPG_PRIVATE_KEY"
echo "   Value: (paste content of $OUTPUT_DIR/private-key.asc)"
echo ""
echo "2. Keep private-key.asc SECURE — it's the key to signing your packages."
echo "   Move it somewhere safe (not in the repo):"
echo "   mv $OUTPUT_DIR/private-key.asc ~/.secure/fcitx5-skey-private-key.asc"
echo ""
echo "3. The public key (key.asc) will be published on GitHub Pages."
echo ""
echo "4. Run: gh secret set APT_GPG_KEY_EMAIL --body '$KEY_EMAIL'"
echo "   (needed to select the right key in CI)"
