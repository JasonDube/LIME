#!/usr/bin/env bash
# Open LIME (the 3D modeling tool). Best-effort rebuild, then launch — if the
# build fails (e.g. source is mid-edit) it still opens the last good binary, so
# the icon always works. Runs from build/ so it resolves shaders/ at runtime.
ROOT="$(cd "$(dirname "$0")" && pwd)"
echo "Building LIME (will launch the last good build if this fails)..."
cmake --build "$ROOT/build" --target lime -j >/dev/null 2>&1 || echo "build failed — launching the last good build"
cd "$ROOT/build"
echo "Launching LIME..."
exec ./lime > "$ROOT/lime_console.log" 2>&1
