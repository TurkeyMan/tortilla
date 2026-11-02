#!/bin/sh
# Packages from build.sh's build: .github/ci/package.sh DEB|RPM. Untagged
# builds carry the version the Arch package would (0.1.0.r49.g329e5a3).
# A tag must name CMakeLists.txt's version (v0.1.0, or v0.1.0-beta): the
# one the About box and tortilla.exe carry.
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
    version=$(echo "$tag" | tr - '~') ;;
  *) version="$base.r$(git rev-list --count HEAD).g$(git rev-parse --short=7 HEAD)" ;;
esac
cmake -B build-ci -DTORTILLA_PACKAGE_VERSION="$version"
(cd build-ci && cpack -G "$1")
mkdir -p dist
ext=$(echo "$1" | tr A-Z a-z)
cp build-ci/packages/*."$ext" dist/
ls -l dist
