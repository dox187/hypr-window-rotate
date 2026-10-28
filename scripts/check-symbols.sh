#!/bin/sh
# Checks that the Hyprland binary exports every function hypr-window-rotate hooks.
# Usage: scripts/check-symbols.sh [path/to/Hyprland]
set -eu

BIN="${1:-$(command -v Hyprland)}"
SYMBOLS='Desktop::View::CWindow::realToReportSize(
CSurfacePassElement::getTexBox(
Render::IElementRenderer::drawSurface(
Render::IElementRenderer::calculateUVForSurface(
Render::IHyprRenderer::damageSurface(
Desktop::CViewHitTester::windowSurfaceAt(
CSeatManager::setPointerFocus(
CSeatManager::sendPointerMotion('

EXPORTED="$(nm -DC "$BIN")"
missing=0
printf '%s\n' "$SYMBOLS" | while IFS= read -r sym; do
    if printf '%s\n' "$EXPORTED" | grep -qF " $sym"; then
        echo "ok      $sym"
    else
        echo "MISSING $sym"
        exit 1
    fi
done || missing=1

[ "$missing" -eq 0 ] && echo "all hook targets found in $BIN" || { echo "some hook targets are missing in $BIN" >&2; exit 1; }
