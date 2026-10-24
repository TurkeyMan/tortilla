#!/bin/sh
# Build dependencies on Debian/Ubuntu (run as root inside the CI container).
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates git cmake ninja-build g++ pkg-config \
  libgit2-dev libgtk-3-dev libgtk-4-dev qt6-base-dev \
  dpkg-dev file
