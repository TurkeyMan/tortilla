# Releasing Tortilla

A release is a tag on master. Pushing a `v*` tag runs the Build workflow
(`.github/workflows/build.yml`), which builds and tests every package and, once
they all pass, publishes them as a GitHub release. Nothing else publishes:
untagged builds only leave their packages as workflow artifacts.

## The version

The version lives in one place: the `project(tortilla VERSION x.y.z)` line in
`CMakeLists.txt`. Everything else follows it:

- the About box (`tortilla about`), through `TORTILLA_VERSION`;
- tortilla.exe's version resource (Explorer's Details tab), as `x.y.z.0`;
- the Windows installer and zip, through `.github/ci/build-windows.ps1`;
- the .deb and .rpm packages, through `.github/ci/package.sh`;
- the Arch packages, through `pkgver()` in `packaging/arch/PKGBUILD`.

`vcpkg.json` has a `version-string` too; vcpkg needs one, and nothing reads it,
but keep it the same.

A tag names that version: `v0.1.0` for 0.1.0, or `v0.1.0-beta`, `v0.1.0-rc.1`
for a pre-release of it (published as a pre-release on GitHub). The Linux job
fails if the tag's version (before any `-suffix`) isn't CMakeLists.txt's.

Untagged builds are snapshots after the version: `0.1.0.r240.g1a2b3c4`
(commits since the start, and the commit). Package managers sort them after
`0.1.0`, so bump the version in CMakeLists.txt soon after a release, before
snapshots of the next one go out.

## Cutting a release

1. **Check forge is ready.** Every branch meant for the release is merged
   into forge, and its latest Build run is green:

   ```sh
   gh workflow run build.yml --ref forge
   gh run watch "$(gh run list --workflow build.yml --branch forge --limit 1 --json databaseId -q '.[0].databaseId')"
   ```

   Try the packages from that run (see *Checking the packages*, below), on
   Windows and on at least one Linux desktop.

2. **Set the version and the changelog**, on forge, as one commit:
   - `CMakeLists.txt`: `project(tortilla VERSION x.y.z ...)`, and the same in
     `vcpkg.json`;
   - `CHANGELOG.md`: a `## x.y.z` section with what changed since the last
     release.

3. **Merge forge into master**, and push:

   ```sh
   git switch master
   git pull --ff-only
   git merge --ff-only origin/forge     # or a plain merge, if master has moved
   git push origin master
   ```

   The push to master runs the Build workflow again; wait for it to pass.

4. **Tag the merged commit**, and push the tag:

   ```sh
   git tag -a vx.y.z -m "Tortilla x.y.z"
   git push origin vx.y.z
   ```

5. **What CI does with the tag.** The Build workflow runs on it:
   - `debian x86_64`, `debian arm64`: build, test, and the .deb packages;
   - `fedora x86_64`, `fedora arm64`: the same, and the .rpm packages;
   - `arch x86_64`: makepkg builds, tests and packages from
     `packaging/arch/PKGBUILD`;
   - `windows x86_64`: MSVC with the static runtime, the tests, a check that
     tortilla.exe and tortilla-shell.dll import nothing but Windows' own DLLs,
     then `tortilla-x.y.z-windows-x86_64.zip` and
     `tortilla-x.y.z-windows-x86_64-setup.exe`;
   - `Release` (only for a `v*` tag, once all of the above pass): downloads
     every job's packages and runs `gh release create vx.y.z` with all of them
     attached, titled "Tortilla x.y.z", with notes generated from the merged
     pull requests and commits. A tag with a `-suffix` makes a pre-release.

   Follow it with `gh run watch`.

6. **Replace the generated notes** with the changelog's section:

   ```sh
   awk '/^## x.y.z/{on=1; next} /^## /{on=0} on' CHANGELOG.md > notes.md
   gh release edit vx.y.z --notes-file notes.md
   ```

7. **Check the release** (below), then announce it: the website's install
   section points at <https://github.com/TurkeyMan/tortilla/releases/latest>.

## Checking the packages

```sh
mkdir rel && cd rel
gh release download vx.y.z          # or: gh run download <run id>, for a branch's run
```

- **Names**: every file carries `x.y.z` (no `.r123.g...`): the Windows zip and
  setup, `tortilla*_x.y.z_{amd64,arm64,all}.deb`,
  `tortilla*-x.y.z-1.{x86_64,aarch64}.rpm` and
  `tortilla*-x.y.z-1-{x86_64,any}.pkg.tar.zst`.
- **Windows zip**: `Tortilla/bin/` holds only tortilla.exe, tortilla-shell.dll,
  tortilla.ico and the `menu/` and `badges/` icons; `Tortilla/translations/`
  the .po files; and README.md, LICENSE and NOTICES.md. Both programs import
  only Windows' DLLs:

  ```sh
  x86_64-w64-mingw32-objdump -p Tortilla/bin/tortilla.exe | grep 'DLL Name'
  ```

- **Windows install**, on a clean machine with Git for Windows: run the setup,
  then Explorer's right-click menu has Tortilla, a repository's files have
  badges, `tortilla about` says x.y.z, and tortilla.exe's Properties > Details
  say x.y.z.0. Uninstall leaves nothing in Program Files.
- **Linux packages**: rapport is Tortilla's own copy, not a system library:

  ```sh
  dpkg-deb -c tortilla-rapport_x.y.z_amd64.deb   # .../lib/x86_64-linux-gnu/tortilla/librapport.so.0
  dpkg-deb -x tortilla-core_x.y.z_amd64.deb root
  dpkg-deb -x tortilla-rapport_x.y.z_amd64.deb root
  readelf -d root/usr/bin/tortilla | grep RUNPATH # $ORIGIN/../lib/x86_64-linux-gnu/tortilla
  ldd root/usr/bin/tortilla | grep rapport        # resolves inside root/
  ```

  Then install `tortilla` (or `tortilla-gtk4`, `tortilla-qt`, `tortilla-gtk3`)
  and a file manager's package on Debian or Ubuntu, Fedora and Arch, and check
  the log opens, the About box says x.y.z, and the file manager's menu shows
  after restarting it (`nautilus -q`, ...).

## When something's wrong

Before anyone has downloaded it, a release can be withdrawn and cut again
from a fixed commit:

```sh
gh release delete vx.y.z --cleanup-tag --yes
git tag -d vx.y.z
```

Fix it on forge, merge into master again, and tag. Once a release has been
downloaded, don't reuse its version: release x.y.(z+1) instead.
