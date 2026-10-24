#!/bin/sh
# Build dependencies on Fedora (run as root inside the CI container).
set -eu
dnf install -y \
  git cmake ninja-build gcc-c++ pkgconf-pkg-config rpm-build \
  libgit2-devel gtk3-devel gtk4-devel qt6-qtbase-devel \
  nautilus-devel nemo-devel caja-devel Thunar-devel kf6-kio-devel
