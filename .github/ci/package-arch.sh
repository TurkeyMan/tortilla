#!/bin/sh
# The Arch packages, from packaging/arch/PKGBUILD (run as root inside the
# archlinux container; makepkg itself won't run as root).
set -eu
pacman -Syu --noconfirm --needed base-devel git cmake ninja libgit2 gtk3 gtk4 qt6-base \
  libnautilus-extension nemo caja thunar kio wayland-protocols
useradd -m builder
chown -R builder .
git config --system --add safe.directory "$PWD"
su builder -c 'cd packaging/arch && makepkg --noconfirm'
mkdir -p dist
cp packaging/arch/*.pkg.tar.zst dist/
rm -f dist/*-debug-*  # makepkg.conf's debug symbols
ls -l dist
