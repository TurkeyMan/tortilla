#!/bin/sh
# The package from build.sh's build: .github/ci/package.sh DEB|RPM. One
# "tortilla" with everything in it (cmake/packaging.cmake), named for the
# download: dist/tortilla-<version>-debian-arm64.deb, ...-fedora-x86_64.rpm.
# Untagged builds carry the version the Arch package would
# (0.1.0.r49.g329e5a3). A tag must name CMakeLists.txt's version (v0.1.0, or
# v0.1.0-beta): the one the About box and tortilla.exe carry.
set -eu
git config --global --add safe.directory "$PWD"
base=$(sed -n 's/^project(tortilla VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
case "${GITHUB_REF:-}" in
  refs/tags/v*)
    tag=${GITHUB_REF#refs/tags/v}
    if [ "${tag%%-*}" != "$base" ]; then
      echo "::error::Tag v$tag doesn't match CMakeLists.txt's version, $base" >&2
      exit 1
    fi
    shown=$tag
    version=$(echo "$tag" | tr - '~') ;;
  *) version="$base.r$(git rev-list --count HEAD).g$(git rev-parse --short=7 HEAD)"
     shown=$version ;;
esac
case "$(uname -m)" in
  aarch64) arch=arm64 ;;
  *) arch=$(uname -m) ;;
esac

ext=$(echo "$1" | tr A-Z a-z)
if [ "$1" = DEB ]; then
  distro=debian
  # The app's and rapport core's libraries; not the backends' or the
  # extensions', which load only where their toolkit or file manager is.
  stage=$(mktemp -d)
  for c in core tortilla; do DESTDIR="$stage" cmake --install build-ci --component $c; done
  elf=$(find "$stage" -type f -exec sh -c '[ "$(head -c4 "$1" | tail -c3)" = ELF ]' _ {} \; -print)
  private=$(dirname "$(find "$stage" -name 'librapport.so.0' | head -n1)")
  mkdir "$stage/debian"
  printf 'Source: tortilla\n\nPackage: tortilla\n' > "$stage/debian/control"
  # shellcheck disable=SC2086 # one argument per file
  deps=$(cd "$stage" && dpkg-shlibdeps -O -l"$private" --ignore-missing-info $elf |
         sed -n 's/^shlibs:Depends=//p')
  rm -rf "$stage"
  if [ -z "$deps" ]; then
    echo "::error::dpkg-shlibdeps found no dependencies for the app" >&2
    exit 1
  fi
  echo "Depends (shared libraries): $deps"
  cmake -B build-ci -DTORTILLA_PACKAGE_VERSION="$version" -DTORTILLA_DEB_DEPENDS="$deps"
else
  distro=fedora
  cmake -B build-ci -DTORTILLA_PACKAGE_VERSION="$version"
fi
rm -rf build-ci/packages
(cd build-ci && cpack -G "$1")
set -- build-ci/packages/*."$ext"
if [ $# -ne 1 ]; then
  echo "::error::Expected one package, got: $*" >&2
  exit 1
fi
mkdir -p dist
out="dist/tortilla-$shown-$distro-$arch.$ext"
cp "$1" "$out"

# It installs, with what this distribution has: nothing of ours it needs is
# missing, and no file manager or toolkit beyond one is required.
if [ "$distro" = debian ]; then
  dpkg-deb -f "$out" Depends
  apt-get install -y --no-install-recommends "./$out"
  dpkg -s tortilla | grep -q '^Status: install ok installed'
else
  rpm -qp --requires "$out"
  if rpm -qp --requires "$out" | grep -v '^(' | grep -iE 'gtk|qt|nautilus|nemo|caja|thunar|kf6|kio'; then
    echo "::error::The package requires a toolkit or file manager library" >&2
    exit 1
  fi
  dnf install -y "./$out"
fi
ls -l dist
