#!/bin/sh
# The Arch package, from packaging/arch/PKGBUILD (run as root inside an Arch
# Linux or Arch Linux ARM container; makepkg itself won't run as root), named
# for the download: dist/tortilla-<version>-arch-x86_64.pkg.tar.zst, or -arm64.
set -eu
pacman -Syu --noconfirm --needed base-devel git cmake ninja libgit2 gtk3 gtk4 qt6-base \
  libnautilus-extension nemo caja thunar kio wayland-protocols
id builder >/dev/null 2>&1 || useradd -m builder
chown -R builder .
git config --system --add safe.directory "$PWD"
su builder -c 'cd packaging/arch && makepkg --noconfirm'
rm -f packaging/arch/*-debug-*.pkg.tar.zst  # makepkg.conf's debug symbols
set -- packaging/arch/tortilla-*.pkg.tar.zst
if [ $# -ne 1 ]; then
  echo "::error::Expected one package, got: $*" >&2
  exit 1
fi
case "${GITHUB_REF:-}" in
  refs/tags/v*) shown=${GITHUB_REF#refs/tags/v} ;;
  *) shown=$(pacman -Qip "$1" | sed -n 's/^Version *: \(.*\)-[0-9]*$/\1/p') ;;
esac
case "$(uname -m)" in
  aarch64) arch=arm64 ;;
  *) arch=$(uname -m) ;;
esac
mkdir -p dist
out="dist/tortilla-$shown-arch-$arch.pkg.tar.zst"
cp "$1" "$out"
# It installs, with what Arch has.
pacman -U --noconfirm "$out"
ls -l dist
