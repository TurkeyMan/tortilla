# Tortilla — design notes

(Formerly "pengit"; renamed to Tortilla on 2026-10-22.)

Carried over from a discussion in the IfritView workspace (2026-10-21/22).
Nothing is built yet. This records the decisions and reasoning so far.

## Goal

A native-feeling TortoiseGit-style git GUI for Linux. The log window is the
priority; rebase and settings dialogs follow.

Motivation: RabbitVCS's log view is poor compared to TortoiseGit's, and it is
Python/GTK, which feels sluggish (per-row interpreter callbacks, a dbus hop to
a Python service, parsing `git` subprocess output). Native C++ is the bar.

## Rejected options

- **Graft TortoiseGit's log into RabbitVCS.** The slowness is Python, not the
  graph; a better cell renderer doesn't fix it.
- **Port TortoiseGit wholesale.** It is MFC throughout (CDialog, DDX, .rc,
  CString, owner-draw CListCtrl, COM shell extension). That is a rewrite, not
  a port, and most of the code is Explorer integration that doesn't apply.
- **wxWidgets.** It wraps native widgets (Win32, Cocoa, GTK) but picks the
  backend at compile time. On Linux that is effectively GTK only; wxQt is
  immature. No runtime choice between Qt and GTK.

Still worth a look before/while building: Gittyup, gitg, QGit (for reference),
and TortoiseGitProc.exe `/command:log` under Wine as a stopgap.

## Architecture

Two parts:

1. **A toolkit-agnostic UI layer** (a narrow wx-style widget layer, but the
   backend unit is the *toolkit*, chosen at runtime).
2. **The git app** on top of it.

### Runtime backend selection

- Core executable links no toolkit.
- Backends are plugins (`ui_qt.so`, `ui_gtk.so`, later Win32/Cocoa) that
  implement a C++ abstract interface; loaded with `dlopen`.
- Pick from `XDG_CURRENT_DESKTOP`, overridable by env var/flag; fall back to
  the other backend if loading fails.
- Never load both toolkits in one process (main-loop conflicts).
- Package backends separately so GNOME installs don't pull in Qt.
- Precedent: LibreOffice's VCL plugins (gtk3/gtk4/qt5/qt6/kf5/kf6).
- Same pattern as IfritView's planned front-end/plugin split.

### Differences from wxWidgets

- Runtime dispatch via virtual interface, not preprocessor-selected classes.
- Small widget set, driven by what the dialogs need.
- No foundation library: std C++ for strings, containers, threads.
- Modern assumptions: virtual model-pull lists, xdg-desktop-portal dialogs,
  HiDPI, dark mode/accent from the portal, one "post to UI thread" primitive.
- Layout is applied by the backend so spacing/margins are native.

### Widget set (initial)

- Window, modal dialog
- VBox, HBox, Form (label/field pairs), Grid, Splitter, Tabs, Group, Spacer
- Virtual multi-column list: selection, sort, context menu, one
  custom-painted column
- TextView: read-only with coloured spans (diffs) or editable (commit msg),
  monospace option
- Entry, Button, Check, Combo, Label
- Menus/context menus, message box, file/folder dialog via portal
- Post-to-UI-thread for worker results (revwalk, git commands)

### Key design decisions

- **Lists pull from the model**: row count + per-cell text callback +
  "rows changed" notification. Maps to QAbstractItemModel / GListModel.
- **Custom cells get a tiny vector drawing API** (lines, circles, rounded
  rects, text, theme colours), mapped to QPainter and Cairo/GtkSnapshot.
  Used for the graph column and branch/tag pills.
- **Real widgets everywhere except the graph cell**, for keyboard nav,
  accessibility, IME and theming.
- **C++ abstract interface** for plugins (same compiler for all parts; no C ABI).

### Layout description

Declarative tree in C++, no pixel positions, no external markup files.

```cpp
auto ui = Dialog{"Rebase", {
    VBox{
        Form{
            {"Branch",   Label{}.bind(branchName)},
            {"Upstream", Combo{}.bind(upstream)},
        },
        Splitter{Vertical,
            List{rebaseModel}.bind(commitList).stretch(),
            TextView{}.readOnly().monospace().bind(commitMessage),
        }.stretch(),
        HBox{
            Check{"Preserve merges"}.bind(preserveMerges),
            Spacer{},
            Button{"Up"}.onClick(moveUp),
            Button{"Down"}.onClick(moveDown),
        },
        Buttons{Ok("Start rebase"), Cancel},
    }
}};
```

- `stretch()` takes spare space; `Spacer{}` pushes siblings apart.
- `Form` is its own node so each backend applies its label alignment rules.
- Sizes are semantic: `width(40_ch)`, `height(6_lines)`, converted with real
  font metrics and scale.
- Dialog buttons by **role** (Ok/Cancel/Apply/Help); backend orders them
  (QDialogButtonBox, GTK conventions).
- `.bind(x)` fills a typed handle member; no string IDs.

## Git app

- libgit2 in-process for the revision walk (no subprocess parse on the hot path).
- Flat in-memory commit vector; graph lanes computed once per load.
- Lane algorithm: qgit's lanes.cpp (GPL), which TortoiseGit also uses.
- Shell out to `git` for actions (rebase, cherry-pick, reset, revert, ...),
  as TortoiseGit does.
- Launch from file managers (Nautilus/Dolphin service menus, RabbitVCS menu)
  or the shell.

First usable log window: graph + list + branch/tag labels + filter box
(message/author/paths) + commit detail with file list + external diff +
context menu of the common actions.

## Risks

- **GTK4 list performance.** GtkColumnView is virtual but has a history of
  slow column resize/scroll on big models; GtkTreeView is faster but
  deprecated since 4.10. Prototype the GTK log list early.
- **Rebase list**: per-row combo (pick/squash/edit/drop) and drag reorder are
  the hardest to abstract. Fallback: keyboard reorder + context menu.
- **Diff rich text**: keep it to coloured spans + monospace
  (QTextCharFormat / GtkTextTag).

## Build order

1. Define the interface from the three dialogs (log, rebase, settings).
2. Qt backend; build the log window on it.
3. GTK backend for the log window only; fix interface leaks it exposes.
4. Everything else.

Rough size: interface 1–2k lines; each backend a few thousand.

Open question: user's desktop (KDE vs GNOME) decides which backend gets
polish first. Qt is the default first backend.

## Source layout and packaging

The source tree mirrors the packages. Users install the package for their
toolkit, and it pulls in the rest:

```
tortilla-qt   (what the user installs)  Depends: tortilla-core, librapport-qt6
tortilla-gtk3                           Depends: tortilla-core, librapport-gtk3
tortilla-gtk4                           Depends: tortilla-core, librapport-gtk4
tortilla-core   app; links no toolkit   Depends: librapport
                                      Recommends: tortilla-qt | tortilla-gtk4 | tortilla-gtk3
librapport      core: detection, loader Recommends: the backends
librapport-<tk> one backend plugin      Depends: librapport (exact version), the toolkit
```

```
rapport/core/              librapport + librapport-dev
rapport/backends/gtk3/     librapport-gtk3   (GtkTreeView, fixed-height mode)
rapport/backends/gtk4/     librapport-gtk4   (GtkColumnView)
rapport/backends/qt6/      librapport-qt6    (todo)
rapport/backends/gtk-common/  shared GTK source, compiled into both GTK backends
rapport/backends/null/     headless test backend, not packaged
rapport/examples/          demo (rebase dialog), gallery (all widgets + graph list)
tortilla/                  the app
```

Each backend directory builds standalone against an installed rapport
(`find_package(rapport)`), for split distro builds. Backend names carry the
toolkit major version (`gtk3`, `gtk4`, `qt6`); plugins are
`<libdir>/rapport/rapport_<name>.so`. The core/backend interface is C++, so
backends depend on the exact core version.
