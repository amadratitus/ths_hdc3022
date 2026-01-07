#!/usr/bin/env bash
# Official TeX Live installer (CTAN)
# Detect existing install → upgrade, otherwise prompt for scheme
# Minimum schema defaults to small

set -e

TL_BASE="/usr/local/texlive"
ARCH=$(uname -m)-linux

echo "=== TeX Live Official Installer ==="

# Require root
if [[ $EUID -ne 0 ]]; then
  echo "Run this script with sudo:"
  echo "  sudo ./latex.sh"
  exit 1
fi

# Detect existing TeX Live installation (bin/tlmgr, follows symlink)
TL_EXISTING_TLMGR=$(find /usr/local/texlive -path '*/bin/*/tlmgr' 2>/dev/null | sort | tail -n 1)

if [[ -n "$TL_EXISTING_TLMGR" ]]; then
  EXISTING_TL_BIN=$(dirname "$TL_EXISTING_TLMGR")
  EXISTING_TL_YEAR=$(basename "$(dirname "$(dirname "$(dirname "$TL_EXISTING_TLMGR")")")")

  echo "Existing TeX Live installation detected:"
  echo "  Year : $EXISTING_TL_YEAR"
  echo "  Bin  : $EXISTING_TL_BIN"
  echo

  echo "What do you want to do with the existing TeX Live installation?"
  echo "  1) Update (upgrade current TeX Live packages)"
  echo "  2) Remove (uninstall TeX Live completely)"
  echo "  3) Cancel (exit without changes)"
  echo

  read -rp "Choose [1/2/3] (default: 3): " USER_CHOICE
  USER_CHOICE=${USER_CHOICE:-3}

  case "$USER_CHOICE" in
    1)
      echo "Updating TeX Live..."
      "$TL_EXISTING_TLMGR" update --self --all
      echo "Update complete."
      ;;
    2)
      echo "Removing TeX Live installation..."
      rm -rf "$TL_BASE/$EXISTING_TL_YEAR"
      rm -f /etc/profile.d/texlive.sh
      echo "TeX Live removed."
      ;;
    3)
      echo "Process cancelled."
      exit 0
      ;;
    *)
      echo "Invalid choice: $USER_CHOICE"
      exit 1
      ;;
  esac

  # Ensure PATH is configured (only needed after update)
  if [[ "$USER_CHOICE" == "1" ]]; then
    echo
    echo "Ensuring PATH is configured..."
    cat <<EOF > /etc/profile.d/texlive.sh
# TeX Live $EXISTING_TL_YEAR
export PATH=$EXISTING_TL_BIN:\$PATH
EOF
    chmod +x /etc/profile.d/texlive.sh
  fi

  exit 0
fi

echo "No existing TeX Live installation found."
echo

# Prompt for scheme
echo "Select installation scheme:"
echo "  1) small   (~800 MB)  – minimal LaTeX (default)"
echo "  2) medium  (~2–3 GB)  – recommended"
echo "  3) full    (~7–8 GB)  – everything"
echo

read -rp "Choose [small/medium/full] (default: small): " USER_SCHEME
USER_SCHEME=${USER_SCHEME:-small}

case "$USER_SCHEME" in
  small)  SCHEME="scheme-small" ;;
  medium) SCHEME="scheme-medium" ;;
  full)   SCHEME="scheme-full" ;;
  *)
    echo "Invalid selection: $USER_SCHEME"
    exit 1
    ;;
esac

echo
echo "Selected scheme: $SCHEME"
echo

echo "[1/7] Installing prerequisites..."
apt update -y
apt install -y perl wget xz-utils tar fontconfig

echo "[2/7] Creating temporary workspace..."
WORKDIR=$(mktemp -d)
cd "$WORKDIR"

echo "[3/7] Downloading TeX Live installer..."
wget -q https://mirror.ctan.org/systems/texlive/tlnet/install-tl-unx.tar.gz

echo "[4/7] Extracting installer..."
zcat install-tl-unx.tar.gz | tar xf -

INSTALL_DIR=$(find . -maxdepth 1 -type d -name "install-tl-*" | head -n 1)
cd "$INSTALL_DIR"

echo "[5/7] Installing TeX Live ($SCHEME)..."
perl ./install-tl \
  --no-interaction \
  --paper=letter \
  --scheme="$SCHEME"

echo "[6/7] Setting PATH system-wide..."
# Determine year dynamically
TL_YEAR=$(ls -1 "$TL_BASE" | grep -E '^[0-9]{4}$' | sort -n | tail -n 1)
cat <<EOF > /etc/profile.d/texlive.sh
# TeX Live $TL_YEAR
export PATH=$TL_BASE/$TL_YEAR/bin/$ARCH:\$PATH
export MANPATH=$TL_BASE/$TL_YEAR/texmf-dist/doc/man:\$MANPATH
export INFOPATH=$TL_BASE/$TL_YEAR/texmf-dist/doc/info:\$INFOPATH
EOF
chmod +x /etc/profile.d/texlive.sh

echo "[7/7] Cleanup..."
rm -rf "$WORKDIR"

echo
echo "=== Installation complete ==="
echo "Restart your shell or run:"
echo "  source /etc/profile"
echo
echo "Verify:"
"$TL_BASE/$TL_YEAR/bin/$ARCH/pdflatex" --version | head -n 2
sleep 1
echo "Write scientific documents with love and LaTeX!"
echo "======================================================="
echo "  Automated by ths_hdc3022/latex.sh $(echo "© 2026") "
echo "======================================================="
echo "You can now use 'tlmgr' to manage packages."
