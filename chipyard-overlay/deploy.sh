#!/bin/bash
# deploy.sh — Copy systolic mesh overlay files into a Chipyard workspace.
#
# Usage:
#   ./chipyard-overlay/deploy.sh [CHIPYARD_DIR]
#
# If CHIPYARD_DIR is not supplied, defaults to $CHIPYARD_DIR env var,
# or $HOME/chipyard.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_SRC="$SCRIPT_DIR/generators/chipyard/src/main/scala"

CHIPYARD="${1:-${CHIPYARD_DIR:-$HOME/chipyard}}"
DEST="$CHIPYARD/generators/chipyard/src/main/scala"

if [ ! -d "$CHIPYARD" ]; then
  echo "ERROR: Chipyard directory not found at $CHIPYARD"
  echo "Usage: $0 [/path/to/chipyard]"
  exit 1
fi

echo "==> Deploying systolic overlay into $CHIPYARD"

# --- 1. Copy new files ---
for f in SystolicMesh.scala SystolicConfigs.scala; do
  echo "  COPY  $f"
  cp -v "$OVERLAY_SRC/$f" "$DEST/$f"
done

# --- 2. Apply DigitalTop.scala patch (if not already applied) ---
PATCH_FILE="$OVERLAY_SRC/DigitalTop.scala.patch"
if [ -f "$PATCH_FILE" ]; then
  echo "  PATCH DigitalTop.scala"
  cd "$CHIPYARD"
  if git apply --check "$PATCH_FILE" 2>/dev/null; then
    git apply "$PATCH_FILE"
    echo "  Patch applied successfully."
  else
    echo "  Patch already applied or conflicts detected — skipping."
    echo "  (Verify DigitalTop.scala manually if needed.)"
  fi
else
  echo "  WARNING: No DigitalTop.scala.patch found — skipping."
fi

echo ""
echo "==> Done. Overlay deployed to $CHIPYARD"
