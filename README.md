# Tortilla
A native, fast git client that lives in your file manager.

<p align="center">
  <img src="data/icons/hicolor/256x256/apps/tortilla.png" width="256" alt="Tortilla's mascot: a turtle with a taco for a shell">
</p>

Tortilla is git where you already are. Right-click a file or folder in
Windows Explorer, or in Nautilus, Dolphin, Nemo, Caja, Thunar or PCManFM on
Linux, and commit, diff, blame or open its history from there. Behind that
menu is a complete history client: a commit graph that scrolls through years
of history without a stutter, interactive rebase, blame, bisect, worktrees and
the rest. And it knows your forge: pull requests and their CI results sit on
your branches, from GitHub, GitLab, Gitea and Forgejo, and you can create,
merge and close them without leaving the log. We think that makes it the most
capable file-manager-integrated git client on any platform.

![The log window: the commit graph with branch and tag labels, the commit's details and its changed files](docs/screenshot.png)

## Why it's different

- **Native, for real.** Tortilla draws with each desktop's own toolkit: Win32
  on Windows; GTK 4, GTK 3 or Qt 6 on Linux, picked to match your desktop when
  it starts. Your theme, fonts, dark mode and scaling apply, because they're
  the toolkit's own. It's not a web page pretending to be an application.
- **Instant.** Written in C++ on [rapport](https://github.com/TurkeyMan/rapport),
  with libgit2 for reading the repository and git for the work. It opens at
  once, keeps up with a 240 Hz display, and anything slow runs off the UI
  thread.
- **Forge-aware.** A branch with a pull request shows its number and a CI mark
  on its label: passed, failed or running, and whether it can be merged. Hover
  for each check; right-click to open, merge or close it.
- **No setup to sign in.** If the GitHub CLI is signed in, Tortilla uses its
  sign-in as it is. Or sign in through the browser, or with a token, kept in
  your system keychain. Public repositories on github.com, gitlab.com and
  codeberg.org need no sign-in at all.
- **At home if you know TortoiseGit.** Its dialogs and menus, so the muscle
  memory carries over, now on Linux too.

## Features

- **File manager**: TortoiseGit's context menu in Explorer and six Linux file
  managers, with an icon on every entry and each entry's place (top level,
  submenu or hidden) up to you. Badges on file and folder icons show Clean,
  Modified, Conflicted and Added at a glance, in Explorer, Nautilus, Nemo,
  Caja and Dolphin.
- **Log**: the commit graph with branch, tag and remote labels; filters, date
  ranges and a Show menu (current branch, all, local, pull requests, my
  branches, branch tips only, only what's not merged); drag a label onto a
  commit to move the branch, double-click to switch. File and folder history
  that follows renames, and author pictures.
- **Sync**: the log's Fetch button fetches every remote side by side,
  fast-forwards branches you aren't on, and tells you when yours is behind;
  its badge says what a sync would bring before you click.
- **Pull requests**: on their branches' labels with CI marks and tooltips;
  Create Pull Request (offering only valid targets, forks included), Merge and
  Close.
- **Commit** and **Check for modifications**: pick files, amend, set the
  author or date, sign off, and restore a file after committing to split a
  change.
- **History tools**: interactive rebase (pick, squash, edit, skip, drag to
  reorder, add commits from a log), blame with a slider through the file's
  history, bisect, reflog, stash list, Format Patch, Export, the Repository
  Browser and Browse References.
- **Branches and the rest**: branch, tag, switch, merge, reset, stash,
  worktrees and submodules, with the right questions asked when the working
  tree has changes; conflicts in your merge tool.
- **Diff**: a built-in unified diff viewer, or your own tool (WinMerge, Beyond
  Compare, Meld, KDiff3 and more, detected for you); images in IfritView.
- **Remotes**: clone, fetch, pull and push with progress; passwords and host
  keys asked in Tortilla's own window; Git LFS.
- **Accounts**: GitHub, GitLab, Gitea and Forgejo, each checked when you open
  Settings; Settings > Remotes says where you're signed in and why you might
  want to be.

See [CHANGELOG.md](CHANGELOG.md) for the full list. More screenshots, of the
pull request marks, Create Pull Request and Accounts on GTK 4 and Qt 6, are on
[tortillagit.org](https://tortillagit.org).

## Platforms

- **Windows** 10 and 11, x86_64: Win32 throughout, Explorer's context menu and
  icon badges. Tortilla runs git itself, so
  [Git for Windows](https://git-scm.com) needs to be installed.
- **Linux**, x86_64 and arm64: GTK 4, GTK 3 or Qt 6, whichever matches your
  desktop (`--ui=gtk4`, `--ui=gtk3` or `--ui=qt6` to choose; `--which-ui` lists
  what was detected). Context menus for Nautilus, Nemo, Caja, Thunar, Dolphin,
  and PCManFM-Qt and PCManFM; icon badges in Nautilus (beside the icon, or
  after the name in the list), Nemo, Caja and Dolphin. Thunar and PCManFM
  give extensions no way to badge icons.

There's no macOS version.

## Installing

Download the one file for your system from the latest
[GitHub release](https://github.com/TurkeyMan/tortilla/releases/latest), for
x86_64 or arm64. Every
[CI run](https://github.com/TurkeyMan/tortilla/actions/workflows/build.yml)
also builds the same files as artifacts, or you can build from source (below).

On Windows, run the installer (`-x86_64-setup.exe`, or `-arm64-setup.exe` on an
ARM PC). It adds Tortilla to Explorer's context menu for every user, and
restarts Explorer (after asking) so the badges show straight away.

On Linux, there is one package for each of Debian and Ubuntu (.deb), Fedora
(.rpm) and Arch (.pkg.tar.zst): `sudo apt install ./tortilla-*.deb`,
`sudo dnf install ./tortilla-*.rpm` or `sudo pacman -U tortilla-*.pkg.tar.zst`.
It has everything: every user interface (it uses the one that matches your
desktop) and the context menu of Nautilus, Nemo, Caja, Thunar, Dolphin,
PCManFM-Qt and PCManFM. Restart your file manager for the menu to appear. On
Arch you can also build it from a checkout: `cd packaging/arch && makepkg -si`.
A package repository can have the split packages instead (a package per user
interface, file manager and backend): configure with
`-DTORTILLA_PACKAGE_SPLIT=ON`.

Tortilla carries its own copy of rapport, in `/usr/lib/<arch>/tortilla` (or
`/usr/lib64/tortilla`, `/usr/lib/tortilla`): it never replaces, or is
replaced by, another application's.

PCManFM's menu is a set of files rather than an extension, so when Settings
chooses other top-level or hidden entries, Tortilla writes your own copy to
`~/.local/share/file-manager/actions/tortilla-user*.desktop` (and removes it
when the choices go back to the defaults); restart PCManFM-Qt to see the
change. Uninstalling leaves that copy behind; delete those files by hand.

## Running

Mostly from the file manager's menu; from a terminal:

```sh
tortilla [log] [path]               # the log for the repository holding path
tortilla commit [path...]           # the Commit dialog
tortilla repostatus [path...]       # Check for modifications
tortilla rebase [path] [upstream]
tortilla blame file [rev]
tortilla diff [path] [rev] [rev2]   # no rev: the working tree against HEAD
tortilla pullrequest [path] [branch]
tortilla settings [path] [diff|menu|network|remotes|accounts]
```

`--all` starts the log with all branches. `--register-shell` adds the context
menu for just you, without installing; `--unregister-shell` removes it.
`src/main.cpp` lists every command.

## Building

### Windows

Install Visual Studio 2022 17.9 or later (tested; older 2022 releases untested),
or Visual Studio 2026, with the Desktop development with C++ workload,
including its bundled CMake tools, and Git for Windows. Visual Studio 2026
has not been tested locally. Clone over HTTPS:

```powershell
git clone --recursive https://github.com/TurkeyMan/tortilla.git
cd tortilla
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1` finds the newest Visual Studio installation with C++ tools and uses
its bundled CMake, even if an older CMake comes first on PATH. It builds Release
in `build\` and prints the path to `tortilla.exe`. An ordinary PowerShell is
enough; no Developer shell setup is needed. Alternatively, open the folder in
Visual Studio, or use a Developer PowerShell or Developer Command Prompt:

```sh
cmake --version
cmake -S . -B build
cmake --build build --config Release
```

CMake must be **3.25 or newer**. If `cmake --version` reports an older version,
use `build.ps1`, use Visual Studio's CMake, or update the CMake on PATH.

CMake downloads and builds a pinned libgit2 release when no installed package
is found. No SSH key, vcpkg or pkg-config is required. Vcpkg is optional:
`build.ps1` uses it when `VCPKG_ROOT` names a bootstrapped full checkout, keeping
its installation and caches under `build\vcpkg\`; Visual Studio's bundled
registry-free vcpkg tool is skipped. An explicitly supplied vcpkg toolchain also
continues to work with the raw CMake commands. Set
`TORTILLA_FETCH_LIBGIT2=OFF` to require an installed package.

### Linux

You need CMake 3.25, a C++23 compiler, libgit2, and the development files
for GTK 3, GTK 4 and/or Qt 6.

```sh
git clone --recursive https://github.com/TurkeyMan/tortilla.git
cd tortilla
cmake -S . -B build -G Ninja
cmake --build build
build/bin/tortilla log /path/to/a/repo
```

[docs/RELEASING.md](docs/RELEASING.md) says how a release is cut and checked.

## Licence

GPLv3: see [LICENSE](LICENSE), and [NOTICES.md](NOTICES.md) for third-party
notices.

## Credits

Tortilla is built on [rapport](https://github.com/TurkeyMan/rapport), the
native UI library it shares with IfritView (MIT). It follows
[TortoiseGit](https://tortoisegit.org)'s design, with thanks to everyone who
made it.
