#!/bin/sh
# plugprobe installer: latest GitHub release binary for this machine.
# Usage: curl -fsSL https://raw.githubusercontent.com/lucianodato/plugprobe/main/install.sh | sh
#    or: sh install.sh [DEST-DIR]   (default: ~/.local/bin)
set -eu
REPO=lucianodato/plugprobe
DEST=${1:-$HOME/.local/bin}
BIN=plugprobe
case "$(uname -s)" in
  Darwin) SUFFIX=macos-universal ;;
  Linux)  SUFFIX=linux-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*) SUFFIX=windows-x86_64.exe; BIN=$BIN.exe ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
URL=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
  | grep -o "https://[^\"]*$SUFFIX\"" | tr -d '"' | head -n 1) || URL=""
[ -n "$URL" ] || {
  echo "no published release with a *$SUFFIX asset yet (tag v*.*.* first)" >&2
  exit 1
}
mkdir -p "$DEST"
curl -fsSL -o "$DEST/$BIN" "$URL"
chmod +x "$DEST/$BIN"
"$DEST/$BIN" --version
case ":$PATH:" in *":$DEST:"*) :;; *)
  echo "installed to $DEST — add it to PATH (e.g. export PATH=\"$DEST:\$PATH\")" ;;
esac
