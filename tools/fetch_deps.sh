#!/usr/bin/env bash
# fetch_deps.sh - Install dependencies on Linux / macOS for building audio_extractor.
#
# This project links FFmpeg (libavformat/libavcodec/libswresample/libavutil)
# and libcurl, all via their C APIs (no external CLI processes).
#
# Debian / Ubuntu:
#   sudo apt-get install -y cmake g++ pkg-config \
#        libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
#        libcurl4-openssl-dev
#
# macOS (Homebrew):
#   brew install cmake ffmpeg curl pkg-config
#
# Then build:
#   cmake -S . -B build
#   cmake --build build
#
# (No third_party/ binaries are needed on these platforms; CMake uses
#  pkg-config to locate the system FFmpeg and curl.)

set -e
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

if [ "$(uname)" = "Darwin" ]; then
    if command -v brew >/dev/null 2>&1; then
        brew install cmake ffmpeg curl pkg-config
    else
        echo "Please install Homebrew first: https://brew.sh" >&2
        exit 1
    fi
elif [ -f /etc/debian_version ]; then
    $SUDO apt-get update
    $SUDO apt-get install -y cmake g++ pkg-config \
        libavformat-dev libavcodec-dev libswresample-dev libavutil-dev \
        libcurl4-openssl-dev
else
    echo "Unsupported platform. Install FFmpeg + libcurl dev packages manually." >&2
    exit 1
fi

echo "Dependencies installed. Build with:"
echo "  cmake -S . -B build && cmake --build build"
