#!/bin/bash
set -e
cd "$(dirname "$0")"

HASH=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")
VERSION=$(grep 'APP_VERSION = ' gui.cpp | head -1 | sed 's/.*"\(.*\)".*/\1/')
echo "Building temp-watch v${VERSION} (${HASH})"

g++ -std=c++17 -O2 \
    -DBUILD_HASH="\"${HASH}\"" \
    -o temp-watch gui.cpp \
    imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp \
    imgui/backends/imgui_impl_glfw.cpp imgui/backends/imgui_impl_opengl3.cpp \
    -I imgui -I imgui/backends \
    $(pkg-config --cflags --libs glfw3 gl) 2>&1 | grep -v "^gui.cpp.*warning"

echo "Done → temp-watch v${VERSION} (${HASH})"
