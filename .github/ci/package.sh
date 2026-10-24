#!/bin/sh
# Packages from build.sh's build: .github/ci/package.sh DEB|RPM. Untagged
# builds carry the version the Arch package would (0.1.r49.g329e5a3).
set -eu
git config --global --add safe.directory "$PWD"
base=$(sed -n 's/^project(tortilla VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
case "${GITHUB_REF:-}" in
  refs/tags/v*) version=$(echo "${GITHUB_REF#refs/tags/v}" | tr - '~') ;;
  *) version="$base.r$(git rev-list --count HEAD).g$(git rev-parse --short=7 HEAD)" ;;
esac
cmake -B build-ci -DTORTILLA_PACKAGE_VERSION="$version"
(cd build-ci && cpack -G "$1")
mkdir -p dist
ext=$(echo "$1" | tr A-Z a-z)
cp build-ci/packages/*."$ext" dist/
ls -l dist
