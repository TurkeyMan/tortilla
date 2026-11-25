Tortilla, a TortoiseGit-style Git client for Windows and Linux file managers.

## Installing

Download the one file for your system from **Assets** below.

**Windows:** `tortilla-@VERSION@-windows-x86_64-setup.exe`, or `-arm64-setup.exe` on an ARM PC (or the zip, for a portable copy). The installer isn't code-signed yet, so SmartScreen will warn: choose **More info**, then **Run anyway**.

**Linux:** one package has everything: every user interface (it uses the one that matches your desktop) and the context menu of Nautilus, Nemo, Caja, Thunar, Dolphin and PCManFM. Restart your file manager after installing for the menu to appear.

| | x86_64 | arm64 |
|---|---|---|
| Debian, Ubuntu | `tortilla-@VERSION@-debian-x86_64.deb` | `tortilla-@VERSION@-debian-arm64.deb` |
| Fedora | `tortilla-@VERSION@-fedora-x86_64.rpm` | `tortilla-@VERSION@-fedora-arm64.rpm` |
| Arch | `tortilla-@VERSION@-arch-x86_64.pkg.tar.zst` | `tortilla-@VERSION@-arch-arm64.pkg.tar.zst` |

Install with `sudo apt install ./tortilla-*.deb`, `sudo dnf install ./tortilla-*.rpm` or `sudo pacman -U tortilla-*.pkg.tar.zst`.

Changes in this release: @COMMITS@
