#!/bin/sh
# Configure, build with every toolkit backend, and run the tests.
set -eu
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release | tee build-ci.log
if grep -E -- '^-- rapport: skipping' build-ci.log | grep -v win32; then
  echo "a toolkit backend is missing; install its -dev package in .github/ci/deps-debian.sh" >&2
  exit 1
fi
cmake --build build-ci --parallel "$(nproc)"

# The tests make their own repositories, so git needs an identity.
git config --global user.name "Tortilla CI"
git config --global user.email ci@tortilla.invalid
for t in tortilla-tests tortilla-rebase-tests; do
  scratch=$(mktemp -d)
  "build-ci/bin/$t" "$scratch"
done
build-ci/bin/tortilla-shell-probe .
