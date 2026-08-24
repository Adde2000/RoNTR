#!/usr/bin/env bash
# Build the TeamSpeak 3 plugin as a native Linux .so.
# Run on Linux from the repo root:  ./scripts/build-linux-plugin.sh
# Requires: g++, git.
set -euo pipefail
cd "$(dirname "$0")/.."

SDK=third_party/ts3client-pluginsdk
[ -d "$SDK" ] || git clone --depth 1 https://github.com/teamspeak/ts3client-pluginsdk "$SDK"

mkdir -p ts3-plugin/dist
g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden \
    -static-libstdc++ -static-libgcc \
    -I"$SDK/include" -Its3-plugin/src \
    ts3-plugin/src/plugin.cpp \
    -o ts3-plugin/dist/ron_tactical_radio_linux_amd64.so -lpthread

echo "Built: ts3-plugin/dist/ron_tactical_radio_linux_amd64.so"
echo "Install: cp ts3-plugin/dist/ron_tactical_radio_linux_amd64.so ~/.ts3client/plugins/"
