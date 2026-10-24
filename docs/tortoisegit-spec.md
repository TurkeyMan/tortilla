# TortoiseGit Log and Rebase dialogs: reference spec for tortilla

Everything here was read from TortoiseGit's source and manual. Nothing comes from memory.
Where a fact could not be established from source, the text says so.

## 0. Sources and citation keys

TortoiseGit `master` was shallow-cloned at commit `7338078f8ddd924b8cddee35f512f2286072136d` (2026-09-27).
Line numbers refer to that commit.

| Key | File |
|---|---|
| **RC** | `src/Resources/TortoiseProcENG.rc` (English dialog templates and accelerators) |
| **POT** | `Languages/Tortoise.pot` (the canonical English text of every `IDS_*` string; the citation gives the pot line of the `#. Resource IDs:` comment) |
| **GLLB** | `src/TortoiseProc/GitLogListBase.cpp` |
| **GLLB.h** | `src/TortoiseProc/GitLogListBase.h` |
| **GLLA** | `src/TortoiseProc/GitLogListAction.cpp` |
| **LD** | `src/TortoiseProc/LogDlg.cpp` |
| **LD.h** | `src/TortoiseProc/LogDlg.h` |
| **RD** | `src/TortoiseProc/RebaseDlg.cpp` |
| **RD.h** | `src/TortoiseProc/RebaseDlg.h` |
| **GSLC** | `src/Git/GitStatusListCtrl.cpp` (the changed-files list, used by both dialogs) |
| **GSLC.h** | `src/Git/GitStatusListCtrl.h` |
| **COL** | `src/TortoiseProc/Colors.cpp` / `Colors.h` |
| **CM** | `src/TortoiseProc/ColumnManager.cpp` / `.h` |
| **LANES** | `src/TortoiseProc/lanes.h` / `lanes.cpp` (graph lane algorithm, derived from QGit by Marco Costalba) |
| **GIT** | `src/Git/Git.cpp` |
| **MAN-LOG** | `doc/source/en/TortoiseGit/tgit_dug/dug_log.xml`, published as https://tortoisegit.org/docs/tortoisegit/tgit-dug-showlog.html |
| **MAN-REB** | `doc/source/en/TortoiseGit/tgit_dug/dug_rebase.xml`, published as https://tortoisegit.org/docs/tortoisegit/tgit-dug-rebase.html |
| **CTX** | `src/TortoiseShell/ContextMenu.cpp` (the Explorer context menu; `CM` is already taken by ColumnManager) |
| **MI** | `src/TortoiseShell/MenuInfo.cpp` (the menu table) |
| **MI.h** | `src/TortoiseShell/MenuInfo.h` |
| **GLB** | `src/TortoiseShell/Globals.h` (`TGitContextMenuEntries` bits and `ITEMIS_*` flags) |
| **SC** | `src/TortoiseShell/ShellCache.cpp` (registry settings read by the shell extension) |
| **SC.h** | `src/TortoiseShell/ShellCache.h` (default menu masks) |
| **SE** | `src/TortoiseShell/ShellExt.cpp` |
| **SE.h** | `src/TortoiseShell/ShellExt.h` |
| **EC** | `src/TortoiseShell/ExplorerCommand.cpp` (Windows 11 sub-commands) |
| **IO** | `src/TortoiseShell/IconOverlay.cpp` |
| **RS** | `src/TortoiseShell/resourceshell.rc` (English strings and icon files of the shell extension; the same strings also appear in POT) |
| **TGP** | `src/Git/TGitPath.cpp` |
| **GAD** | `src/Git/GitAdminDir.cpp` |
| **SLF** | `src/TortoiseProc/Settings/SetLookAndFeelPage.cpp` (the three context-menu settings pages) |
| **SF** | `src/TortoiseGitSetup/StructureFragment.wxi` (installer registry entries) |
| **APPX** | `src/TortoiseGitSetup/SparsePackage/AppxManifest.xml` (Windows 11 sparse package) |
| **MAN-ADM** | `doc/source/en/TortoiseGit/tgit_app_admins.xml` |
| **TP** | `src/TortoiseProc/TortoiseProc.cpp` (command-line path parsing) |
| **CMDH** | `src/TortoiseProc/Commands/Command.h` |
| **CMDC** | `src/TortoiseProc/Commands/Command.cpp` |
| **DIFFC** | `src/TortoiseProc/Commands/DiffCommand.cpp` |
| **RSC** | `src/TortoiseProc/Commands/RepoStatusCommand.cpp` |
| **SCC** | `src/TortoiseProc/Commands/ShowCompareCommand.cpp` |
| **CC** | `src/TortoiseProc/Commands/CommitCommand.cpp` |
| **CHD** | `src/TortoiseProc/ChangedDlg.cpp` (Check for modifications) |
| **CD** | `src/TortoiseProc/CommitDlg.cpp` |
| **AU** | `src/TortoiseProc/AppUtils.cpp` |
| **GD** | `src/TortoiseProc/GitDiff.cpp` |
| **GSLCH** | `src/TortoiseProc/GitStatusListCtrlHelpers.cpp` (file-list sort comparator) |
| **SCE** | `src/Utils/MiscUI/SciEdit.cpp` (commit message editor) |
| **PP** | `src/TortoiseProc/ProjectProperties.cpp` / `.h` |

Labels are quoted exactly as they appear in the resources. `&` marks the mnemonic (the underlined access key).

Geometry is given in **dialog units (DLU)**. The dialogs use `FONT 9, "Segoe UI"`, so 1 horizontal DLU = avgCharWidth/4 and 1 vertical DLU = charHeight/8. At 96 DPI with Segoe UI 9pt, a DLU is roughly 1.75 px horizontally and 1.875 px vertically; treat this as an approximation. Rectangles are written as `x,y,w,h`.

---

## 1. Log dialog (`IDD_LOGMESSAGE`)

### 1.1 Window

- Template: `IDD_LOGMESSAGE DIALOGEX 0,0,422,265`. Caption "Log Messages". Styles: resizable (`WS_THICKFRAME`) with minimize and maximize boxes; `WS_EX_APPWINDOW`. [RC:210-214]
- Actual title: `<path> - Log Messages - TortoiseGit`, where the path is the repository root, or the file or folder the log was opened for. The path is compacted to fit 80 characters. [LD:610-619; `AppUtils.cpp:3518-3548` FormatWindowTitle]
- The dialog saves and restores its size and position (`EnableSaveRestore(L"LogDlg")`). [LD:383]
- In plain "view" mode the OK button is hidden and Cancel is relabelled "OK". In "select revision" mode (when another dialog uses the Log to pick commits), OK is visible and enabled according to the selection rules. [LD:424-438, 631-648]

### 1.2 Control layout (top to bottom, left to right)

All controls come from RC:216-242. The anchors (how each control moves when the window is resized) come from `AddMainAnchors()` [LD:2094-2126].

**Row 1: filter bar (y=5)**

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Ref/branch selector | `IDC_STATIC_REF` | static text, `SS_NOTIFY`, end-ellipsis; it behaves like a hyperlink (`m_staticRef.SetURL`) | text is the current ref (see 1.9.1) | 7,5,88,15 | top-left |
| "From:" label | `IDC_FROMLABEL` | static | "From:" | 99,5,82,15 (it extends under the date picker; the extra width is used for the blue highlight) | top-left |
| From date | `IDC_DATEFROM` | date picker (`SysDateTimePick32`, right-aligned) | – | 122,5,57,15 | top-left |
| "To:" label | `IDC_TOLABEL` | static | "To:" | 188,5,19,15 | top-left |
| To date | `IDC_DATETO` | date picker | – | 207,5,55,15 | top-left |
| Search/filter box | `IDC_SEARCHEDIT` | `CFilterEdit`: an edit box with a magnifier "info" icon (`IDI_LOGFILTER`, 19×19) that opens the options menu, and a red X cancel button (14×14) | cue text, see 1.9.2 | 264,5,45,15 | **stretches** (top-left to top-right) |
| Jump type | `IDC_LOG_JUMPTYPE` | drop-down list | items in 1.9.6 | 312,6,70,(64 drop) | top-right |
| Jump up | `IDC_LOG_JUMPUP` | icon button (`IDI_JUMPUP`, green up arrow) | – | 385,5,14,14 | top-right |
| Jump down | `IDC_LOG_JUMPDOWN` | icon button (`IDI_JUMPDOWN`) | – | 400,5,14,14 | top-right |

**Main area: three vertical panes separated by two horizontal splitters**

| Control | ID | Type | Rect (DLU) | Anchor |
|---|---|---|---|---|
| Commit list | `IDC_LOGLIST` | owner-data report list view (`LVS_OWNERDATA`, `LVS_SHOWSELALWAYS`, multi-select) | 7,24,407,57 | top-left to middle-right |
| **Splitter 1** | `IDC_SPLITTERTOP` | `CSplitterControl` | 7,81,407,5 | middle |
| Commit details | `IDC_MSGVIEW` | read-only RichEdit (`RichEdit50W`) with a vertical scrollbar | 7,86,407,56 | middle |
| Gravatar picture | `IDC_PIC_AUTHOR` | static picture box, hidden by default | 361,86,53,49 (top-right corner of the details pane) | middle-right |
| **Splitter 2** | `IDC_SPLITTERBOTTOM` | `CSplitterControl` | 7,142,407,4 | middle |
| Changed files | `IDC_LOGMSG` | `CGitStatusListCtrl` (report list view) | 7,147,407,57 | middle-left to bottom-right |

**Bottom area**

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Status line | `IDC_LOGINFO` | borderless read-only edit (so the text can be selected) | see 1.10 | 7,208,407,12 | bottom, stretches |
| Whole-project checkbox | `IDC_WHOLE_PROJECT` | auto checkbox | "Show &Whole Project" | 7,221,138,10 | bottom-left |
| All-branches checkbox | `IDC_LOG_ALLBRANCH` | **tri-state** checkbox (`BS_3STATE`) | "&All Branches" | 7,232,138,10 | bottom-left |
| File path filter | `IDC_FILTER` | `CFilterEdit` with info icon `IDI_FILTEREDIT` and cancel X | cue "   Filter paths" | 148,225,208,14 | bottom, stretches |
| Help | `IDHELP` | button | "Help" | 364,225,50,14 | bottom-right |
| Refresh | `IDC_REFRESH` | button | "Refresh" | 7,244,76,14 | bottom-left |
| Statistics | `IDC_STATBUTTON` | button, initially disabled | "S&tatistics" | 86,244,64,14 | bottom-left |
| Walk behavior | `IDC_WALKBEHAVIOUR` | menu button that always shows a drop arrow; drawn "checked" (pushed) while any walk option is active | "Walk Be&havior" | 153,244,80,14 | bottom-left |
| View | `IDC_VIEW` | menu button that always shows a drop arrow | "&View" | 236,244,50,14 | bottom-left |
| OK | `IDOK` | default button, hidden in view mode | "OK" | 305,244,52,14 | bottom-right |
| Cancel/OK | `IDCANCEL` | button, relabelled "OK" in view mode | "Cancel" | 364,244,50,14 | bottom-right |
| Progress bar | `IDC_PROGRESS` | progress bar, hidden by default | – | 87,246,212,10 | bottom, stretches |

While the log loads, the progress bar is shown in place of Statistics, Walk Behavior and View, which are hidden. At the end of loading those three come back and the progress bar is hidden. [LD:542-550, 571-574]

The Refresh button sits bottom-left, not with OK and Cancel.

### 1.3 Splitter behaviour

- The splitters are `CSplitterControl` instances. Dragging splitter 1 calls `DoSizeV1(delta)`: the log list grows by `delta`, and the message view shrinks first down to a minimum height of 20 px (scaled). Any remaining delta then shrinks the file list, with splitter 2 moved accordingly. [LD:2029-2060]
- Dragging splitter 2 calls `DoSizeV2(delta)`: the message view absorbs the change down to 20 px, then the log list. [LD:2062-2092]
- Ranges: splitter 1 can move between `listTop+20px` and `filesBottom-(2*20+10)px`. Splitter 2 can move between `listTop+(2*20+10)px` and `filesBottom-20px`. Here `MIN_CTRL_HEIGHT` = 20 px and `MIN_SPLITTER_HEIGHT` = 10 px, both DPI-scaled. [LD:36-37, 2182-2197]
- When the window is resized, `OnSize` re-clamps all three panes so each stays at least 20 px tall and the file list ends 3 DLU above `IDC_LOGINFO`. [LD:2949-3069]
- Both splitter positions are saved to the registry (`LogDlgSizer1` and `LogDlgSizer2`, unscaled y values) and restored at start-up. [LD:386-419, 1268-1282]
- The Gravatar box is kept top-aligned with the message view (`MoveToSameTop`). When Gravatar is enabled, the message view's right edge stops at the picture's left edge; otherwise the view spans the full width. [LD:2016-2027, 3126-3153]

### 1.4 Commit list columns

Defined in `CGitLogListBase::InsertGitColumn()` [GLLB:321-336]. The column enum order is in GLLB.h:288-303 and the header strings come from POT.

`columnWidth` = `ScaleX(5 + 16*4)` = **69 px** at 96 DPI [GLLB:318]. Each `ColumnDefinition` is `{id, title, visibleByDefault, available, defaultWidth}` [CM.h:23-30].

| # | Column | Header text | Log dialog: visible by default / available | Default width | Contents |
|---|---|---|---|---|---|
| 0 | GRAPH | "Graph" [POT:4989] | **yes** / yes | 69 px | Branch graph, owner-drawn (1.6) |
| 1 | REBASE | "REBASE" [POT:7350] | no / **no** (Rebase dialog only) | 69 | Rebase action name |
| 2 | ID | "ID" [POT:5126] | no / **no** (Rebase dialog only) | 69, right-aligned [GLLB:342] | Sequence number |
| 3 | HASH | "SHA-1" [POT:7966] | **no** / yes | 69 | Full 40-hex hash [GLLB:1548-1550] |
| 4 | ACTION | "Actions" [POT:1550] | **yes** / yes | `2*5 + 5*16` = **90 px** | Up to five 16×16 icons (below) |
| 5 | MESSAGE | "Message" [POT:6164] | **yes** / yes | 200 px (`LOGLIST_MESSAGE_MIN_WIDTH`) | Ref labels, then the subject |
| 6 | AUTHOR | "Author" [POT:1929] | **yes** / yes | 69 | Author name |
| 7 | DATE | "Date" [POT:3332] | **yes** / yes | 69 | Author date |
| 8 | EMAIL | "Email" [POT:4029] | no / yes | 69 | Author email |
| 9 | COMMIT_NAME | "Commit Name" [POT:2734] | no / yes | 69 | Committer name |
| 10 | COMMIT_EMAIL | "Commit Email" [POT:2718] | no / yes | 69 | Committer email |
| 11 | COMMIT_DATE | "Commit Date" [POT:2714] | no / yes | 69 | Committer date |
| 12 | BUG | "Bug-ID" [POT:2190] | yes / yes, but only when bugtraq properties are configured | 69 | Bug ID extracted from the message |
| 13 | SVNREV | "SVN Rev" [POT:8022] | yes / yes, but only when the repository has a git-svn dir | 69 | Revision parsed from the `git-svn-id:` trailer [GLLB:1455-1485] |

So the default Log dialog columns, left to right, are **Graph | Actions | Message | Author | Date**.

Column details:

- **Header right-click** opens a menu with "Reset columns" [POT:7767], a separator, then one checkable item per available column. Column order and widths are persisted per list (registry key `...\StatusColumns\logloglist`). [CM:561-606, 88-160]
- **Header left-click does not sort.** It opens the modal "Log commit ordering" dialog (`IDD_LOGORDERING`). That dialog is a combo labelled "Commit Ordering:" with these entries: "Chronological reversed (git default)", "--topo-order (TortoiseGit default)", "--date-order", "--author-date-order". [LD:2828-2848, 2479-2485; `LogOrdering.cpp:47-54`; RC:1984-1992]
- **Date text** comes from `CLoglistUtils::FormatDateAndTime(date, DATE_SHORTDATE|LONGDATE, true, relative)`. The format is short by default (`LogDateFormat`, default TRUE). Relative times are optional (`RelativeTimes`, default FALSE); when on, the tooltip shows the absolute date. The Working-tree row has an empty date. [GLLB:80-92, 1561-1566, 3684-3697]
- **Actions column** draws icons into fixed slots: slot n is at x = `left + 5 + n*16`, top-aligned. The slots, in order, are: **Modified** (`IDI_ACTIONMODIFIED`), **Added or copied** (`IDI_ACTIONADDED`), **Deleted** (`IDI_ACTIONDELETED`), **Replaced/renamed** (`IDI_ACTIONREPLACED`), and **Conflicted/unmerged** (`IDI_ACTIONCONFLICTED`). A slot stays empty when its action is absent. While the commit's diff is still being computed, a single "fetching" icon (`IDI_ACTIONFETCHING`) is drawn; if the computation fails, an error icon (`IDI_ACTIONERROR`) is drawn. [GLLB:1356-1425] The tooltip reads "Actions:" followed by the action names, one per line. [GLLB:3698-3747] The manual says "four different icons" [MAN-LOG:95-118], but the code draws five, the fifth being conflict.
- **Row height**: the list installs a fake image list 1 px wide and `SM_CYSMICON` (16 px) tall, so rows are at least 16 px high even when the font is smaller. [GLLB:344-352] (`MeasureItem` returns 50 px, but it only applies to owner-draw-fixed lists, which this list is not, so it has no effect.)
- **Full-row select**, info-tips, double-buffering and sub-item images are enabled through the extended style. [GLLB:262-265]
- The message column normally shows only the subject. The setting `FullCommitMessageOnLogLine` appends the body with newlines replaced by spaces. [GLLB:1487-1501]

### 1.5 Row styling (all rows)

From `OnNMCustomdrawLoglist` [GLLB:1050-1130]:

- **The HEAD commit is drawn in a bold font.** A commit "highlighted" through the selection history is drawn in italics, and one that is both is drawn bold italic. [GLLB:1097-1110; MAN-LOG:41 "The line shown in bold indicates the HEAD commit"]
- The normal text colour is the window text colour, and the background is the window colour. (The rebase-specific colours are in section 2.4.)
- When a filter is active, the matching substrings in Message, SHA-1, Author/Commit Name, Email/Commit Email and Bug-ID are drawn in the **FilterMatch colour, RGB(200,0,0)**, with no background change. [GLLB:3938-3997, 1427-1447; COL:53]
- The tooltip on the Message column shows the full subject, but only for rows that carry ref labels, since the labels may hide part of the text. Hovering over a branch label whose branch has a description (`branch.<name>.description`) shows "Description:" followed by the text, positioned at the mouse. [GLLB:3605-3631, 3674-3683]

### 1.6 Ref labels (drawn before the subject in the Message column)

Code: GLLB:1156-1350 (building the label list) and GLLB:523-657 (drawing). The default colours come from COL:23-54 (registry keys `HKCU\Software\TortoiseGit\Colors\*`).

**Which refs.** `GetMapHashToFriendName` lists every ref with libgit2 `git_reference_foreach` (or `git show-ref -d`). For each commit hash the full ref names are **sorted alphabetically** [GIT:2213-2228]. The labels therefore appear in this order: `refs/bisect/*`, `refs/heads/*` (local branches), `refs/notes/*`, `refs/remotes/*`, `refs/stash`, `refs/tags/*`. Annotated tags match as `refs/tags/X^{}`. **HEAD itself is never a label**; HEAD is shown only by the bold row.

**Short names** [GIT:3016-3070]: local branches drop `refs/heads/`, remotes drop `refs/remotes/` (giving `origin/master`), tags drop `refs/tags/`, and the stash is shown as "stash". Bisect refs are shown as the configured good or bad term (default "good"/"bad") or "skip". Notes drop `refs/notes/`, and any other ref drops `refs/`.

**Default colours**

| Ref type | Colour key | Default RGB | Notes |
|---|---|---|---|
| Current branch (local branch == checked-out branch) | CurrentBranch | **(200,0,0)** dark red | text automatically white (dark background, see below) |
| Other local branch | LocalBranch | **(0,195,0)** green | |
| Remote branch | RemoteBranch | **(255,221,170)** peach | |
| Tag (lightweight or annotated) | Tag | **(255,255,0)** yellow | annotated tags have an arrow tip, see below |
| Stash | Stash | **(128,128,128)** grey | |
| Bisect good | BisectGood | **(0,100,200)** blue | |
| Bisect bad | BisectBad | **(255,0,0)** red | |
| Bisect skip | BisectSkip | **(192,192,192)** light grey | **bug:** reads the registry value `Colors\BisectBad` [COL:51] |
| Notes ref | NoteNode | **(160,160,0)** olive | shown only when "Other refs" is enabled |
| Other refs | OtherRef | **(224,224,224)** | |
| Submodule "super-project-pointer", and the mine/theirs labels during a submodule merge conflict | hard-coded | **(246,153,253)** pink | [GLLB:1324-1338] |

Colours pass through `CTheme::GetThemeColor(..., true)`, which adapts them in dark mode.

**Shape**

- The label is a rectangle covering the full row height. Its width is the text width plus 8 px, and the text is centred. If the label would run past the column's right edge, it is clipped there and the text is left-aligned. Labels are separated by 1 px, and a space-width gap separates the last label from the subject. [GLLB:543-565, 651, 461-465]
- **Plain labels**: a solid fill plus a two-step 3D bevel. The outer edge is Lighten(c,100) on the top and left and Darken(c,100) on the bottom and right; the inner edge (1 px inset) is Lighten(c,50) and Darken(c,50). [GLLB:571-583] `Lighten` and `Darken` mix the colour toward white or black by amount/255. [COL:85-109]
- **Local branch with an upstream (tracking) configured**: drawn as a **rounded rectangle** (corner radius 4) with a drop shadow. The shadow is Darken(c,100), drawn offset +2,+2 and deflated by 1; the face is drawn on top, offset back by -2,-2. No bevel is drawn. [GLLB:393-413, 567-569] When remote labels are shown and the upstream ref points at the same commit, the remote label is placed **immediately after** its local branch and also drawn rounded (`hasTracking` stays true). [GLLB:1208-1253] The manual describes this the same way [MAN-LOG:153].
- **Annotated tag**: the rectangle has an 8 px **triangular apex** on its right side, filled with the tag colour. The upper edge is drawn with a 2 px pen in Lighten(c,50) and the lower edge in Darken(c,50). [GLLB:585-605; MAN-LOG:147]
- **Text colour**: the label background counts as "dark" when `R*30 + G*59 + B*11 <= 12800`, which is luminance at or below about 128. Dark labels use the highlight text colour (white); others use the window text colour (black). On a selected row in the classic, non-themed style, the label colour is mixed toward the highlight colour (factor 150/255). [GLLB:534-539, 608-637]
- **Optional settings**:
  - `DrawTagsBranchesOnRightSide` draws the labels after the subject instead of before it [GLLB:448-518].
  - `SymbolizeRefNames` (default off) shortens remote names. With a single remote, the label becomes `/branch`, with a small "upstream" glyph (a T-shaped arrow drawn by `DrawUpstream`) inside the label, drawn heavier on HEAD. When the upstream branch has the same name as the local branch, the label becomes `/≡` or `remote/≡`. [GLLB:1234-1248, 1275-1282, 415-429, 639-646]
- Labels can be toggled per type with View → Labels (1.9.5) through `m_ShowRefMask` [GLLB:1201-1318].
- The on-screen position of each label is recorded (`m_RefLabelPosMap`), so a right-click on a particular label can tailor the context menu to that ref (1.11). [GLLB:648-649, 3751-3787]
- Rows whose rebase status is "done" do not draw labels. [GLLB:1170]

### 1.7 Graph column

The lanes are computed by `Lanes` (a QGit algorithm) [LANES] in `CLogDataVector::setLane` (`LogDataVector.cpp:272-342`). They are drawn by `DrawGraph` and `paintGraphLane` [GLLB:663-1048].

- **The graph is drawn only when** `m_ShowFilter & FILTERSHOW_MERGEPOINTS` is set and **no text filter is active**. With a search filter active, or in "Show labeled commits only" mode, the column is left blank. [GLLB:1135-1154]
- The graph is **not drawn on the Working-tree row** (empty hash). [GLLB:984-985, 1147-1148]
- **Lane geometry**: lane width `lw = 3*rowHeight/4`, and lanes are laid out left to right from x=0. For a lane spanning `x1..x2`, the centre is `m=(x1+x2)/2` and the node radius is `r = (x2-x1)*NodeSize/30`. `NodeSize` defaults to 10 (registry `Graph\LogNodeSize`), so r ≈ lw/3 and the node diameter is about two-thirds of the lane width. The line width defaults to 2 px (`Graph\LogLineWidth`). [GLLB:109-110, 667-670, 1008-1025]
- **Lane colours**: lane i is drawn in `BranchLine(1 + i%8)`. The defaults are: 1 black (0,0,0), 2 red (255,0,0), 3 green (0,255,0), 4 blue (0,0,255), 5 grey (128,128,128), 6 olive (128,128,0), 7 teal (0,128,128), 8 purple (128,0,128). [COL:41-48; LANES `COLORS_NUM = 8`] The "active colour" belongs to the first lane that is a merge (`isMerge`), or lane 0 if there is none. Horizontal connectors and the arcs of joins, heads and tails use this active colour, with a linear gradient from the active colour to the lane's own colour. [GLLB:1001-1012, 707-767, 840-888]
- **Lane types** [LANES enum]: EMPTY, ACTIVE, NOT_ACTIVE, MERGE_FORK, MERGE_FORK_R, MERGE_FORK_L, MERGE_FORK_L_INITIAL, JOIN, JOIN_R, JOIN_L, HEAD, HEAD_R, HEAD_L, TAIL, TAIL_R, TAIL_L, CROSS, CROSS_EMPTY, INITIAL, BRANCH, UNAPPLIED, APPLIED, BOUNDARY, BOUNDARY_C, BOUNDARY_R, BOUNDARY_L.
- **Vertical segments** [GLLB:785-838]:
  - Full height (top to bottom): ACTIVE, the MERGE_FORK variants, NOT_ACTIVE, the JOIN variants and CROSS.
  - From the centre down: BRANCH and HEAD_L.
  - From the top to the centre: INITIAL, MERGE_FORK_L_INITIAL, the BOUNDARY variants and TAIL_L.
- **Horizontal segments**, drawn in the active colour [GLLB:842-888]:
  - Full width: MERGE_FORK, BOUNDARY_C, JOIN, HEAD, TAIL, CROSS and CROSS_EMPTY.
  - Left edge to centre: MERGE_FORK_R and BOUNDARY_R.
  - Centre to right edge: MERGE_FORK_L, MERGE_FORK_L_INITIAL, BOUNDARY_L, HEAD_L and TAIL_L.
- **Curves**: JOIN, JOIN_R, HEAD and HEAD_R draw a quarter arc (270° to 360°) from the left neighbour down into the lane. JOIN_L draws the mirrored arc. TAIL and TAIL_R draw an arc in the upper half. All arcs are anti-aliased. [GLLB:707-770]
- **Node shapes** [GLLB:898-963; MAN-LOG:121-136]:
  - **Filled circle**: an ordinary commit (ACTIVE, INITIAL, BRANCH).
  - **Filled square**: a merge or fork point (the MERGE_FORK variants). The manual: "The squares indicate merges and branchings."
  - **Hollow circle**: BOUNDARY (boundary commits, only drawn when `LogIncludeBoundaryCommits` is on).
  - **Square**: BOUNDARY_C, BOUNDARY_R and BOUNDARY_L.
  - **Minus bar / plus sign**: UNAPPLIED and APPLIED (a horizontal bar, or a bar plus a vertical bar, 2 px thick).
  - **Collapsed ("rolled-up") commits**: the circle or square is drawn **hollow** (a 1 px outline), and the line segments stop at the node's rim instead of passing through its centre. [GLLB:791-829, 907-924; MAN-LOG:312]
- First-parent mode computes the lanes with `onlyFirstParent`. [GLLB:996-997]

### 1.8 "Working tree changes" pseudo-row

- When enabled, it is row 0. It is a `GitRevLoglist` with an **empty hash** and parent = HEAD. Subject: "Working tree changes" [POT:10495]. The body starts as "\nFetching Status..." [POT:4455], and once the working-tree diff is computed it becomes "\n%d files changed" [POT:243]. [GLLB.h:239-256; GLLB:144-173]
- It is shown when the registry setting `LogIncludeWorkingTreeChanges` (default TRUE) is on, the repository is not bare, and the caller has not suppressed it (`m_bShowWC`, default true). [LD:302; LD.h:230]
- Its Actions icons are the union of all working-tree file actions. Its Date column is empty, and no graph is drawn on it.
- When selected, the file list shows the working-tree changes, with **unversioned files** in their own group when View → "Show Unversioned Files" is checked (default from the setting `AddBeforeCommit`, TRUE). [LD:1026-1032, 78-79]
- The context menu for this row is different (see 1.11.2).
- The status-bar revision count excludes this row. [LD:2909-2913]

### 1.9 Filters and options

#### 1.9.1 Ref selector (`IDC_STATIC_REF`, top-left)

- **Text**: `<All Branches>`, `<Basic Refs>` or `<Local Branches>` when one of those modes is on. Otherwise it shows the range or ref with `refs/heads/` etc. stripped. For HEAD it shows the symbolic ref, or `<No branch>` when HEAD is detached. The tooltip holds the full text. [LD:3291-3335; POT:1405,1413,1417,1421]
- **Left-click** opens the Browse References dialog to pick a ref or range, and adds the result to a most-recently-used list (default 5 entries). [LD:162, 3271-3289, 471-475]
- **Right-click menu** [LD:1368-1456]:
  1. "Browse References" (**default item**, drawn bold) [POT:2174]
  2. `HEAD -> "branch"` (just `HEAD` when detached)
  3. `FETCH_HEAD` (disabled when FETCH_HEAD does not exist)
  4. "All" [POT:1661]
  5. "All basic refs" [POT:1677]
  6. "All local branches" [POT:1681]

  Items 4 to 6 are disabled while Follow renames is on. A separator and then the history entries (truncated to 150 characters) follow. Choosing "All" puts the **All Branches** checkbox into the checked state. Choosing "basic refs" or "local branches" puts it into the **indeterminate** state.

#### 1.9.2 Search box (`IDC_SEARCHEDIT`)

- **Cue text**: three spaces, then "Filter by", then a comma-separated list of the selected fields in this order: Subject, Messages, Paths, Authors, Emails, SHA-1, Refname, Notes, and Bug-IDs (Bug-IDs only when bugtraq is configured). Example: `   Filter by Subject, Messages, Paths, Authors, Emails, SHA-1, Refname, Notes`. Tag Info is selectable as a field but never appears in the cue. [LD:2341-2410]
- **Typing** restarts a 1-second timer; when it expires the log is filtered. Enter applies the filter immediately. Clearing the text, or clicking the X, removes the filter. [LD:3156-3179, 1790-1794, 2312-2338]
- In regex mode, invalid regexes are rejected by a validator, and a tooltip lists the regex syntax ("Regular expressions filter: ..." [POT:7524]). [LD:621-629, 2412-2418]
- **Options menu** (opened by clicking the magnifier icon, positioned at the icon's bottom-left) [LD:2204-2310]. Every item is a checkable toggle:

| Item | Bit |
|---|---|
| "Subject" [POT:8996] | `LOGFILTER_SUBJECT` 0x40 |
| "Messages" [POT:6176] | `LOGFILTER_MESSAGES` 0x01 |
| "Paths" [POT:7008] | 0x02 |
| "Authors" [POT:1949] | 0x04 |
| "Emails" [POT:4037] | 0x100 |
| "SHA-1" | `LOGFILTER_REVS` 0x08 |
| "Refname" [POT:7496] | 0x80 |
| "Tag Info" [POT:9191] | `LOGFILTER_ANNOTATEDTAG` 0x400 |
| "Notes" [POT:6553] | 0x200 |
| "Bug-IDs" [POT:2202], only if bugtraq is configured | 0x20 |
| *separator* | |
| "Toggle filters" [POT:9613] (inverts all the field bits) | |
| "All" (selects all fields) | |
| *separator* | |
| "Use regular expression" [POT:10099] | 0x10 |
| "Case-sensitive" [POT:2325] | 0x800 |

  Defaults: all fields on (`SelectedLogFilters` = 0xFFFF); regex off (`UseRegexFilter`); case-insensitive (`FilterCaseSensitively`). [LD:45-48; GLLB.h:58-71]
- **Simple (non-regex) search syntax** [MAN-LOG:715-764]: space-separated terms that must all match; `-term` excludes a term; `+term` re-includes it; a leading `!` inverts the whole expression; `"quoted phrases"` match literally, and `""` is a literal quote. In regex mode a leading `!` also inverts the result. [MAN-LOG:705]

#### 1.9.3 From / To dates

- Changing a date restarts the log after 10 ms. From is clamped so it is never after To, and vice versa. From means 00:00:00 of the chosen day; To means 23:59:59. [LD:2495-2558]
- After loading, both pickers are set to the time range actually shown (oldest and newest commit), unless an explicit limit was set. [LD:576-584]
- The calendar drop-down shows week numbers and has no "today" button. [LD:334-335, 2940-2947]
- **Default limit indicator**: when a default limit is active ("last N commits/weeks/months/years" or "last selected date", set in Settings → Dialogs 1), the **"From:" label gets a light blue background RGB(174,200,255)**. The background is painted 5 px further left and 3 px further up and down than the label. [LD:332, 488-520; MAN-LOG:689]
- **Right-click on From** [LD:1458-1520]: "No limitation" [POT:6452]; then a separator and the configured limit, e.g. "Last %s commit(s)" / "Last %s week(s)" / "month(s)" / "year(s)" [POT:5787-5802], shown only when one is configured; then a separator and "Configure default" [POT:2871], which opens the settings.

#### 1.9.4 Walk Behavior (menu button) [LD:3352-3415]

Every item is a checkable toggle. The menu opens below the button (`TPM_VERTICAL`, excluding the button rect).

1. "First Parent" [POT:4622]: `--first-parent`
2. "No &merges" [POT:6367]: `--no-merges`
3. "&Follow renames" [POT:647]: enabled only when the log was opened for a single **file**. Turning it on also clears All Branches and disables both the All Branches and Show Whole Project checkboxes. [LD:3211-3234]
4. "Full history" [POT:4776]: `--full-history`
5. *separator*
6. "&Compressed Graph" [POT:443]: `m_ShowFilter` = REFS|MERGEPOINTS, which hides commits that are neither labelled nor merge or fork points.
7. "Show la&beled commits only" [POT:8580]: `m_ShowFilter` = REFS only, which hides the graph.

Items 6 and 7 are mutually exclusive (radio-like); choosing either one again turns it off. The button shows the checked (pushed) state while any of these options is on. [LD:3256-3269, 3413]

#### 1.9.5 View (menu button) [LD:3417-3524]

1. "H&ide Unrelated Changed Paths" [POT:5005]
2. "&Gray Unrelated Changed Paths" [POT:695] (**the default**: `m_iHidePaths = 2` [LD:295]). Items 1 and 2 are mutually exclusive, and both can be off. "Unrelated" means files outside the path the log was opened for; such files are drawn in the grey text colour or hidden. [LD:966-999]
3. *separator*
4. "Show &Unversioned Files" [POT:8428] (applies to the working-tree row)
5. *separator*
6. "&Labels" ► submenu: "&Tags", "&Local branches", "&Remote branches", "Other refs" [POT:1202, 813, 1042, 6806]. Each is a per-repository toggle, default on. [LD:69-76]
7. *separator*
8. "G&ravatar" [POT:4792] (default off; registry `EnableGravatar`)
9. "View Patch" [POT:10298]: toggles a separate patch-view window docked beside the log dialog. The state is persisted in the git config `tgit.logshowpatch`. [LD:1137-1155]

The View button is only momentarily drawn checked, while its menu is open. [LD:3432, 3522]

#### 1.9.6 Navigation (jump) combo and up/down buttons

The combo entries are [LD:365-375]:

- "Author Email"
- "Committer Email"
- "Merge Point"
- "Parent 1"
- "Parent 2"
- "Tag"
- "Tag (FF)"
- "Branch"
- "Branch (FF)"
- "Selection History"

The default is "Author Email".

- **Up** searches upward (toward newer commits) from the selected row for the next row that matches the selected row's value. Examples: the same author email; a commit whose first or second parent is the selected commit; any merge; any tag; any branch. The "FF" variants require the found commit to be a fast-forward of the selected one. **Down** is the mirror image. [LD:2565-2764]
- When nothing more is found, an information box with a "do not show again" option says "No more revisions found." [POT:6456]
- "Selection History" maps Up and Down to Back and Forward in the selection history.
- Alt+Up and Alt+Down trigger the buttons. [RC:3480-3481]

#### 1.9.7 Bottom checkboxes

- **Show Whole Project** clears the path restriction. It is enabled only when a path was given and Follow renames is off. Its state is remembered **per repository**. [LD:67-68, 564, 3543-3562; MAN-LOG:783]
- **All Branches** is a tri-state checkbox: unchecked (the current ref or range), checked (`--all`), or indeterminate (basic refs or local branches, chosen from the ref menu). Clicking it only toggles between unchecked and checked. Its state is remembered **per repository**. [LD:53-65, 3181-3209]

### 1.10 Status line (`IDC_LOGINFO`)

Format [LD:2895-2938; POT:8702]:

```
Showing %1 revision(s), from revision %2 to revision %3 - %4 revision(s) selected, %5 file(s) selected
```

- `%2` is the short hash of the oldest shown commit (the last row); `%3` is the newest (the first row, skipping the working-tree row).
- When exactly one revision is selected, the file statistics are appended after "; " with newlines replaced by spaces. The statistics come from `IDS_STATUSLIST_STATUSLINE1` [POT:10869]:

  ```
  line: %d(+) %d(-) files: modified = N added = N deleted = N replaced = N
  ```

### 1.11 Commit-list context menu

The menu is built fresh on each right-click in `CGitLogListBase::OnContextMenu` [GLLB:1640-2348].

The Log dialog hides these items by default: the rebase actions, "Show log..." (unless the row has refs or Shift is held), Blame, and Blame previous [GLLB:94-100]. Pressing **Shift** while opening the menu shows the "extended" items (marked "Shift" below). [GLLB:1662; MAN-LOG:178]

Definitions used below:

- **WC** means the selected row is the Working-tree row (empty hash).
- **hasWC** means the repository is not bare.
- **merge** means a merge is in progress (`MERGE_HEAD` exists).
- **bisect** means a bisect is in progress.
- **stash** means the row is the stash commit, or the index commit directly below it.
- **refs** means the commit has at least one ref label.
- **parents** is the parent count. When there is more than one parent, the item becomes a submenu with one entry per parent, labelled `Parent N: "first 20 chars of subject..." (shorthash)` [POT:6896].

Separators are only emitted when the group before them produced at least one item. In the order the items are appended:

**Group R: rebase actions** (Rebase dialog only; each is hidden if the row is current or done) [GLLB:1726-1739]

- "Pick" [POT:7020]
- "Squash (with commit below)" [POT:8820]. Not offered on the last (bottom) row, nor on a merge commit unless in cherry-pick mode.
- "Edit"
- "Skip"
- *separator*

**Single selection (selectedCount == 1)**

Group A (diff), when the row is not WC:

- "Compare with &working tree" [POT:2815] (needs hasWC)

When the row is WC:

- "C&ommit..." [POT:2238]
- "Abort &Merge" [POT:1505] (merge only)

Then, for any row:

- "Blame previous revision" [POT:2093]. Hidden by default in the Log dialog; a submenu when there are several parents.
- "Show changes as &unified diff" [POT:8516] (needs hasWC). With several parents it becomes "&Unified diff with" ► containing "All Parents", "Only Merged Files", "Show extra changes after merge", then one entry per parent. [POT:1254,1673,6702,8560]
- "&Compare with previous revision" [POT:439]. A submenu when there are several parents. It becomes the **default (bold)** item when the setting `DiffByDoubleClickInLog` is on.
- "Blame" (hidden by default)
- *separator*

Group B (stash):

- "Stash changes" [POT:8924]: WC row and no merge in progress
- "Stash Pop" [POT:8916] and "Stash List" [POT:8900]: (WC row or stash row) and a stash exists
- *separator*

Group C (bisect):

- "Bisect good", "Bisect bad", "Bisect skip": while bisecting, on a row that is not itself a bisect ref
- "Bisect reset": WC row while bisecting
- *separator*

Group D (WC row only):

- "&Pull..." [POT:994] (no merge in progress)
- "F&etch..." [POT:4320]
- "Submodule Update" [POT:9036] (if the repository has submodules)
- *separator*
- "&Clean up..." [POT:403]
- *separator*

Group E (commit rows, not WC):

- "Show log..." [POT:8617]: when (refs and not stash) or Shift. It is the default item in the RefLog dialog.
- "&Browse repository" [POT:383]
- `&Merge to "<currentBranch>"...` [POT:833]: not HEAD, hasWC, no merge in progress, not stash. If the click was on a ref label, that ref is the one merged.
- `R&eset "<currentBranch>" to this...` [POT:7337]: hasWC, not stash
- "&Switch/Checkout to" [POT:1186]: refs include local branches other than the current one, hasWC, not stash.
  - With one branch the label is `&Switch/Checkout to "name"`. With several it is a submenu of branch names.
  - If the click was on a label, only that branch is offered.
- "Sw&itch/Checkout to this..." [POT:9070]: not HEAD, hasWC, not stash
- "Create Br&anch at this version..." [POT:3182]: not stash. If the click was on a remote label, the new branch is pre-filled from it.
- "Create &Tag at this version..." [POT:3166]: not stash
- `Rebase "<currentBranch>" onto this...(&G)` [POT:7410]: not HEAD, hasWC, no merge in progress, not stash
- "E&xport this version..." [POT:3933]
- "&Revert change by this commit" [POT:1086]: hasWC, no merge in progress, not stash. A submenu with one entry per parent for merge commits.
- "Edit &Notes" [POT:3961]: not stash
- *separator*

**RefLog rows only**: "Delete Ref..." [POT:3375]; "Stash Apply" (for `refs/stash` entries); then a separator.

**Two or more selected** [GLLB:2092-2145]:

- "&Compare revisions" [POT:435]: when exactly 2 are selected, or the selection is contiguous
- Exactly 2 selected:
  - "Show changes as &unified diff" (needs hasWC)
  - three range items: "Show log of A..B", "Show log of B..A", "Show log of A...B" [POT:8604], where A is the short hash of the lower (older) row and B the upper (newer)
  - "Comp&are change sets" [POT:2787]
- *separator*
- "&Revert changes by these commits" [POT:1090] (hasWC, no merge in progress)
- "Bisect skip" (when more than one is selected while bisecting), then a *separator*

**Commit rows (single or multi)** [GLLB:2153-2196]:

- "Com&bine to one commit" [POT:2672]: a contiguous multi-selection that ends at HEAD's first-parent chain, i.e. `HEAD~n..HEAD~m` (hasWC, no merge in progress)
- "Cherr&y Pick this commit..." / "Cherr&y Pick selected commits..." [POT:2468/2464]: not HEAD, hasWC, no merge in progress
- "&Format Patch..." [POT:667]: not stash, and 2 or fewer selected or a contiguous selection
- *separator*

**Bisect start** [GLLB:2198-2202]:

- "Bisect start" [POT:2077]: exactly 2 selected, the first is not WC, not stash, hasWC, no merge or bisect in progress
- *separator*

**Single selection, part 2** [GLLB:2204-2295]:

- "Pus&h..." [POT:7277]: when (refs and not stash) or Shift, and the refs include a local branch or tag (or Shift). Right-clicking on a local branch or tag label turns it into `Push "name"...`.
- "SVN DCommit..." [POT:8010]: after Push, when git-svn is present
- "&Pull...": on the HEAD row (hasWC, no merge in progress)
- Delete:
  - With exactly one ref other than the current branch: `&Delete <fullrefname>` [POT:479], e.g. "Delete refs/heads/foo".
  - With several: "&Delete branch/tag" ► one entry per full ref name, then "All".
  - If the click was on a label, only that ref is offered.
- *separator*
- "Collapse revisions" / "Expand collapsed revisions" [POT:2639/4255]: only when the graph is shown (no filter active, not labelled-only mode) and the row is not WC
- *separator*

**Always present**:

- "Copy to clipboard" [POT:2992] ►
  - "Full data" [POT:4772]
  - "Full commit data without changed paths" [POT:4768]
  - "SHA-1"
  - "Authors (full)" [POT:1953]
  - "Author names" [POT:1941]
  - "Author email addresses" [POT:1937]
  - "Subjects" [POT:9004]
  - "Messages"
  - "Tag/branch names" [POT:9195]: single selection with refs

  The RefLog dialog shows only Full data, SHA-1 and Messages.
- "Search &log messages..." [POT:8199]: opens the Find dialog, same as Ctrl+F
- "Show branches this commit is &on" [POT:8508]: single selection, not WC

**Clipboard formats** [GLLB:2378-2492]:

- Full data:

  ```
  Revision: <hash>
  Author: <name> <email>
  Date: <date>
  Message:
  <message>
  ----
  Notes: ...
  ----
  Tag Info: ...
  ----
  <Action>: <path> (from <old>)
  ```

- Subjects: `* subject\r\n\r\n` per commit.
- Messages: `* full message\r\n\r\n` per commit.
- Hashes, authors and emails: one per line.

#### 1.11.1 Notes on the context menu

- When opened with the keyboard (Shift+F10 or the menu key), the menu is anchored at the centre of the selected row. [GLLB:1653-1659]
- The commit-message pane (`IDC_MSGVIEW`) has its own context menu [LD:1522-1590]:
  - "Cop&y" [POT:2948] (disabled when nothing is selected)
  - *separator*
  - "Copy all information to clipboard" [POT:2964] (copies the whole text)
  - *separator* and "Edit &Notes" (commit rows only)

#### 1.11.2 Changed-files list (bottom pane)

**Columns.** The pane is initialised with `Init(COLEXT|COLSTATUS|COLADD|COLDEL, "LogDlg", POPALL ^ (POPRESTORE|POPCHANGELISTS), no checkboxes, hasWC, allowed = same four)` [LD:308]. The columns are:

| Column | Header | Notes |
|---|---|---|
| Path | "Path" [POT:6996] | always shown; full path relative to the repository, with a system file icon |
| Extension | "Extension" [POT:4296] | |
| Status | "Status" [POT:8948] | action name: Modified, Added, Deleted, Renamed, ... |
| Lines added | "Lines added" [POT:5898] | right-aligned |
| Lines removed | "Lines removed" [POT:5902] | right-aligned |

[GSLC:302-319] Filename, Last Modified, Size and LFS Lock Owner exist in the control but are not allowed in this dialog.

**Text colours** [GSLC:3415-3440; COL]:

| Condition | Colour |
|---|---|
| Unrelated path (View → Gray) | grey text (`COLOR_GRAYTEXT`) |
| Conflicted | (255,0,0) |
| Modified | (0,50,160) |
| Added or copied | (100,0,100) |
| Deleted | (100,0,0) |
| Renamed | (0,0,255) |
| Merged | (0,100,0) |
| Anything else | window text colour |

**Groups.** For a merge commit the list switches to group view: a header per parent, "Diff with parent %d: <shorthash>", plus "Merged Files". The working-tree row uses groups "Modified Files", "Not Versioned Files", "Ignored Files" and "Local changes ignored (...)". [GSLC:3968-4060]

**Placeholder text** while loading: "Fetching changed files..."; on error: "Could not fetch commit or calculate diff."; when empty: "File list is empty" or "File list is empty due to active filter". [LD:955-1019]

**Path filter.** The path filter box (`IDC_FILTER`, cue "Filter paths") hides non-matching files after a 1 s delay (Enter applies at once; Esc clears it). When the log's path filter is active, matches are highlighted in the Path column. [LD:1003-1022, 1795-1816, 2420-2423, 3600-3612]

**Double-click** on a file: on a conflicted file it opens the conflict editor; otherwise it diffs the file against the commit's parent (or between the two revisions when comparing two). [GSLC:2926-2975]

**Context menu** (`OnContextMenuList` [GSLC:1639-2060]), filtered by the popup mask:

- Conflicted file:
  - "&Edit conflicts" [POT:571] (default)
  - "Resolved" [POT:7784]
  - `Resolve conflict using "theirs"` / `"mine"` [POT:7779] (the actual titles come from `GetConflictTitles`)
  - *separator*
- Unversioned or ignored file:
  - "Add" [POT:1563], plus with Shift: "Add as Executable (+x)" and "Add as Symlink"
  - "Commit..."
- Versioned file:
  - "Compare with b&ase" [POT:2819] (default), or "Three way diff" [POT:9593] for files in the merged-files group
  - "Show changes as &unified diff"
  - "Compare with &working tree" (a commit is selected and hasWC)
  - `Compare parent with working tree: <parent title>` [POT:2795]
  - *separator*
- Two revisions being compared: "&Compare revisions" (default) and "Show changes as &unified diff"
- Exactly two files selected: "Compare two files" [POT:2807]
- Working-tree row only:
  - "Commit..."
  - "Re&vert..." [POT:7386]
  - "Skip worktree" [POT:8764]
  - "Assume Unchanged" [POT:1865]
  - "Unflag as skip-worktree and assume-unchanged" [POT:9902]
  - LFS lock/unlock
- Commit row:
  - "R&evert to this revision" [POT:7346] (file not deleted)
  - `Revert to parent revision: <title>` [POT:7842]
- Single selection:
  - "Show &log" [POT:8440]
  - "Show log of submodule" [POT:8609] (directory/submodule)
  - "Show log &before rename/copy" [POT:8596]
  - "&Blame" [POT:359]
- "Export selection to..." [POT:4275] (not deleted)
- Single selection:
  - "Save revision &to..." [POT:8137]
  - "View revision in alternative editor" [POT:10318]
  - "&Open" [POT:922]
  - "Open with..." [POT:6774]
  - "Explore to" [POT:4259]
  - *separator*
  - "Mark for comparison" [POT:6075], then `Diff with "<file:rev>"` [POT:3614] once a file has been marked
- Unversioned or missing files: "&Delete" and "Add to &ignore list" (►, with by-name and by-extension items)
- *separator*
- "Copy to clipboard" ► "full paths", "relative paths", "file/foldernames", "Copy all information to clipboard", and `column '<name>'` when a column was hit [POT:10832, 11004, 10828, 2964, 10791]
- *separator*, then the Windows shell context menu ("Shell") for working-tree files

**Keys** [GSLC:3832-3876]:

- Ctrl+A selects all.
- Ctrl+C and Ctrl+Insert copy relative paths; with Shift added, they copy paths plus status.
- Delete removes unversioned files.

### 1.12 Keyboard and mouse

**Accelerator table `IDR_ACC_LOGDLG`** [RC:3463-3484]:

| Key | Action |
|---|---|
| Ctrl+C, Ctrl+Insert | Copy the selection as "Full data". When the changed-file list has focus, copy its paths instead. Skipped when the focus is in the message view or a filter box. [LD:1779, 3085-3091] |
| Ctrl+F, F3 | "Search log messages" Find dialog |
| Alt+D, Ctrl+L | Focus the search box |
| Ctrl+E | Focus the search box and select all its text |
| F5, Ctrl+F5, Shift+F5 | Refresh |
| Alt+Left / Alt+Right, Browser Back / Forward | Go back or forward in the selection history **and select** the commit |
| Shift+Alt+Left / Shift+Alt+Right | Go back or forward without selecting; scrolls to and italic-highlights the commit |
| Alt+Up / Alt+Down | Jump up / down (navigation combo) |
| Esc | Clear the active filter if there is one, otherwise close the dialog [LD:3590-3598] |

**Other keys and mouse actions** [LD:1776-1842; GLLB:2719-2758]:

- **Enter** in the commit list runs "compare with previous" when `DiffByDoubleClickInLog` is on. In the search box it applies the filter now; in the path filter box it applies that filter now. The OK button only closes the dialog when it has focus. [LD:1597-1598]
- **Ctrl+A** in the list selects all rows.
- **Ctrl+V or Shift+Insert**, anywhere except the filter boxes: if the clipboard holds something that looks like a hash, jump to the matching commit. The search starts below the current row and wraps around. If nothing matches, the window flashes and "No more revisions found." is shown.
- **Mouse X1/X2 (back/forward) buttons** navigate the selection history; holding Shift navigates without selecting.
- **Double-click on a commit** does nothing by default. With `DiffByDoubleClickInLog` on, it compares with the previous revision. [GLLB:2760-2767]
- **Hyperlinks in the message pane**: hashes (7+ hex characters by default) are links that jump to the commit. URLs and bug IDs open externally, and e-mail addresses become `mailto:`. [LD:650-728, 1932-1997]

Selection history: up to 50 entries, with browser-style forward history that is discarded when a new branch of history starts. [GLLB.h:89-157]

### 1.13 Commit-details pane (`IDC_MSGVIEW`)

The pane is filled in `FillLogMessageCtrl` [LD:793-1059]. It is a RichEdit using the "log font", by default **Consolas 9pt** (registry `LogFontName` and `LogFontSize`) [`CommonAppUtils.cpp:360-365`]. For a single selection the text is:

```
SHA-1: <40-hex hash>[, Branch RevNo: N]
[Describe: <git describe>]

* <subject>                    ← subject in bold; "* " prefix controlled by AsteriskLogPrefix (default on)
<body>
[* Notes:                      ← "Notes:" in bold
<notes>]
[* Tag Info:                   ← bold
<annotated tag info>]
```

- Filter matches are coloured with FilterMatch (200,0,0).
- Text between `*...*` becomes **bold**, `^...^` becomes *italic*, and `_..._` becomes underlined (setting `StyleCommitMessages`, default on). [`AppUtils.cpp:731-790`]
- With several rows selected, the pane is empty and the file list shows nothing. With nothing selected, both are cleared.
- The Gravatar picture (if enabled) loads the author's e-mail avatar.

---

## 2. Rebase dialog (`IDD_REBASE`)

The same dialog class (`CRebaseDlg`) also serves as the **Cherry Pick** dialog (`m_IsCherryPick`); the differences are noted below.

### 2.1 Window

- Template `IDD_REBASE DIALOGEX 0,0,417,237`, caption "Rebase", resizable, `WS_EX_APPWINDOW`. [RC:1499-1503]
- The title is `<repo root> - Rebase - TortoiseGit`; in cherry-pick mode the name part is "Cherry Pick". [RD:249, 306]
- Size and position are saved and restored (`EnableSaveRestore(L"RebaseDlg")`). [RD:251]

### 2.2 Control layout

Controls come from RC:1505-1530 and anchors from RD:120-153.

**Row 1 (y≈7)**

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Label | `IDC_REBASE_STATIC_BRANCH` | static | "&Branch:" | 10,9,34,8 | top-left |
| Branch combo | `IDC_REBASE_COMBOXEX_BRANCH` | drop-down list (history combo), **sorted**; lists all local and remote branches; preselects the current branch | – | 48,7,111,(140) | top-left to top-centre |
| Swap button | `IDC_BUTTON_REVERSE` | icon button (`IDI_SWITCHLEFTRIGHT`, ⇄) | – | 163,7,19,15 | top-centre |
| Label | `IDC_REBASE_STATIC_UPSTREAM` | static | "&Upstream:" | 186,9,41,8 | top-centre |
| Upstream combo | `IDC_REBASE_COMBOXEX_UPSTREAM` | drop-down list, sorted; lists all branches (fully qualified) plus tags; preselects `remotes/<remote>/<branch>` tracked by HEAD | – | 230,7,116,(140) | top-centre to top-right |
| Browse | `IDC_BUTTON_BROWSE` | button | "..." | 352,7,19,15 | top-right |
| Onto | `IDC_BUTTON_ONTO` | **push-like checkbox** (toggle button), checked whenever an `--onto` ref is set | "&Onto" | 377,7,32,14 | top-right |

[RD:471-509, 316, 321]

**Commit list**

| Control | ID | Type | Rect (DLU) | Anchor |
|---|---|---|---|---|
| Commit list | `IDC_COMMIT_LIST` | owner-data report list view (a `CGitLogList`, the same class as the log) | 7,25,402,85 | top-left to top-right (the height changes only via the splitter) |

**Row under the list (y≈113)**: these controls move together with the splitter [RD:394-408].

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Set-all split button | `IDC_SPLITALLOPTIONS` | `CMenuButton` (split button: the main face runs the current entry, the arrow picks another) | current entry text, initially "&Pick ALL" | 7,113,102,14 | top-left |
| Up | `IDC_BUTTON_UP` | button | "&Up" | 116,113,33,14 | top-left |
| Down | `IDC_BUTTON_DOWN` | button | "&Down" | 156,113,33,14 | top-left |
| Add | `IDC_BUTTON_ADD` | button (`BS_ICON` style flag; the source sets no icon for it) | "&Add" | 194,113,48,14 | top-left |
| Preserve merges | `IDC_REBASE_CHECK_PRESERVEMERGES` | checkbox | "&Preserve merges" | 249,115,83,10 | top-left (sic) |
| Force rebase | `IDC_REBASE_CHECK_FORCE` | checkbox | "&Force Rebase" | 340,115,66,10 | top-centre (sic) |
| Cherry-picked-from | `IDC_CHECK_CHERRYPICKED_FROM` | checkbox; **cherry-pick mode only**; in that mode it occupies the space where the two checkboxes above are hidden | `&add "cherry picked from"` | 293,115,116,9 | top-right |

The split button entries, in order [RD:227-241]:

- "&Pick ALL"
- "&Squash ALL"
- "&Edit ALL"
- "S&kip unselected"
- "S&quash unselected"
- "E&dit unselected"

[POT:958,1158,563,7950,7954,3921]

**Splitter**: `IDC_REBASE_SPLIT`, a `CSplitterControl` at 7,129,402,6. [RD:379-433]

**Tab area**: `CMFCTabCtrl` (flat style in light mode, 3D in dark mode). It is created in the rectangle of the invisible placeholder `IDC_REBASE_DUMY_TAB` (7,137,402,64) and stretches top-left to bottom-right. [RD:183-247, 3065-3080] Tabs, by index:

| Index | Before start | After start (Continue pressed) |
|---|---|---|
| 0 | "Revision Files" [POT:7893]: changed files of the selected commit; no checkboxes | renamed **"Conflict Files"** [POT:2903]: conflicting and changed working-tree files, **with checkboxes** |
| 1 | "Commit Message" [POT:2730]: Scintilla in the log font, **read-only**; shows the selected commit's message | same control; becomes **editable** in conflict and edit stages |
| 2 | (absent) | **"Log"** [POT:5987]: read-only Scintilla with the git commands and their output, appended line by line. Added and activated when the rebase starts. |

When the rebase finishes, **all tabs except the last (Log) are removed**. [RD:1181-1182]

**Bottom**

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Progress bar | `IDC_REBASE_PROGRESS` | progress bar, range 0..commitCount | – | 7,204,402,7 | bottom, stretches |
| Status text | `IDC_STATUS_STATIC` | static; switches to **white text on a red RGB(255,0,0) background** in the warning state | e.g. "Rebasing... (3/7)" | 7,215,201,8 | bottom, stretches |
| Edit/split checkbox | `IDC_REBASE_SPLIT_COMMIT` | checkbox, hidden and disabled except in the Edit and Squash_Edit stages | "&Edit/Split commit" | 122,219,91,10 | bottom-right |
| Continue | `IDC_REBASE_CONTINUE` | **default** button; its text changes with the stage (2.6) | "Continue" | 213,217,63,14 | bottom-right |
| Abort | `IDC_REBASE_ABORT` | button; tooltip "Recover to the status before rebase" | "Abort" | 285,217,57,14 | bottom-right |
| Help | `IDHELP` | button | "Help" | 351,217,58,14 | bottom-right |
| Post-action button | `IDC_REBASE_POST_BUTTON` | `CMenuButton`, hidden and disabled until Done; then it **replaces the status text** | entries "Show &log", "Restart rebase" (from the Rebase command) | 6,217,75,14 | bottom-left |

[RD:351-361, 1890-1898; `Commands/RebaseCommand.cpp:33-34`]

**Tooltips**:

- Force: "Force the rebase even if the current branch is a descendant of the commit you are rebasing onto. ..." [POT:4699]
- Preserve merges: "This preserves merge commits, however, it has known bugs as re-ordering commits is not working properly ..." [POT:9566]
- Abort: as above. [RD:223-225]
- Both combos: a multi-line tooltip with "Revision: / Author: / Date: / Message:" for the tip commit. [RD:766-803]
- Onto, when set: the onto ref.

**Splitter.** Its range is `listTop+60px` to `tabBottom-80px` (not DPI-scaled). Its position is saved as `RebaseDlgSizer`. Moving the splitter resizes the list and moves the row of buttons and checkboxes plus the tab area. It also raises the dialog's minimum track size so the list cannot be pushed off-screen. [RD:253-271, 394-469]

### 2.3 Commit list columns (rebase configuration)

The Rebase dialog sets `m_IsIDReplaceAction = TRUE`, `m_IsRebaseReplaceGraph = TRUE` and `m_bNoHightlightHead = TRUE`, and the column registry key is `"Rebase"` (persisted as `Rebaseloglist`). [RD:282-289] With the definitions in GLLB:321-336 this gives:

| Column | Visible by default | Available | Content |
|---|---|---|---|
| Graph | – | **no** | |
| **REBASE** | **yes** | yes | Action name: "Pick", "Squash", "Edit", "Skip" (`IDS_PATHACTIONS_*`) [GLLB:275-287, 1542-1545] |
| **ID** | **yes** | yes | Right-aligned sequence number. **The bottom row is 1** and numbers increase upward (`size - rowIndex`). [GLLB:1528-1533, 1551-1554] |
| **SHA-1** | **yes** | yes | full hash |
| Actions | – | **no** | |
| **Message** | **yes** | yes | ref labels and subject, as in the Log |
| **Author** | **yes** | yes | |
| **Date** | **yes** | yes | |
| Email, Commit Name, Commit Email, Commit Date | no | yes | |
| Bug-ID, SVN Rev | conditional | conditional | as in the Log |

So the default Rebase columns, left to right, are **REBASE | ID | SHA-1 | Message | Author | Date**. HEAD is **not** bolded in this list.

**Row order.** The list comes from `git log upstream..branch` in topo order: **newest at the top, oldest at the bottom**. Merges are excluded unless Preserve merges is checked. Commits are **applied from the bottom up** (ascending ID). "Squash" means "fold into the commit below". [RD:599-607; MAN-REB "Rebasing commits takes places from the bottom of the list to the top"]

**Initial action**: Pick for every row. When there is no Preserve merges, any commit that `git cherry upstream branch` reports as already upstream (a `-` line) starts as **Skip**, and the status line shows the **red warning** "Please take a look at the skipped commits" [POT:7081]. [RD:703-755]

**List placeholder text** (drawn centred in the empty list with `ShowText`):

- "Please select branch"
- "Please select upstream"
- `No need to rebase\n"A" equals "B"`
- `Branch "A" behind "B"\n"A" will fastforward to "B"`. In this case Continue reads "Start (FastFwd)".
- `Current branch "A" is up to date\n\nIf you want to force a rebase ... please check below "Force Rebase" checkbox.`
- "Nothing to Rebase"

[RD:521-607; POT:7073,7077,6460,2153,3282,6557]

### 2.4 Row styling by action and state [GLLB:357-391, 1083-1095, 466-505]

| State | Background | Text | Font |
|---|---|---|---|
| Pick | window colour | normal | normal |
| **Squash** | **RGB(156,156,156)** grey | normal | normal |
| **Edit** | **RGB(200,200,128)** khaki (theme-adjusted) | normal | normal |
| **Skip** | window colour | **grey RGB(128,128,128)** | normal |
| **Done** (already applied) | per action | **grey RGB(128,128,128)**; ref labels are no longer drawn | normal |
| **Current** (being applied) | per action | normal | **bold** |

On selected rows the selection colour takes precedence. The custom background fill is skipped for selected rows. [GLLB:368-375]

### 2.5 Changing the per-row action

Actions are a bit field on each row: PICK 0x04000000, SQUASH 0x02000000, EDIT 0x01000000, SKIP 0x00400000, DONE 0x00800000, CURRENT 0x08000000. [GLLB.h:406-417]

**Keyboard** (only while the list has focus, and only while the corresponding menu item is enabled) [RD:805-872]:

| Key | Effect |
|---|---|
| **Space** | Cycle the action of each selected row: **Pick → Skip → Edit → Squash → Pick**. On the bottom row, Edit goes straight to Pick (Squash is not allowed there). [GLLA:1343-1371] |
| **P** | Pick |
| **S** | Skip |
| **Q** | Squash |
| **E** | Edit |
| **U** | Move the selection up (same as the Up button) |
| **D** | Move the selection down |
| Ctrl+A | Select all |
| F5 | Refresh (re-read branches and the list before the start; refresh the conflict list during a conflict) |
| Ctrl+Enter | Press Continue if it is enabled; otherwise focus Abort, or failing that Help |
| Esc | When focus is in the list, a Scintilla editor, a RichEdit or a tab child, the key is re-posted to the dialog, which then aborts. This stops Esc from destroying a tab's child control. |

The manual documents only "space: shifts the state, s: skip, e: edit, p: pick, q: squash". [MAN-REB]

**Mouse**: right-click a row to get the context menu. Its first group is **Pick / Squash (with commit below) / Edit / Skip**, with icons `IDI_PICK`, `IDI_SQUASH`, `IDI_EDIT` and `IDI_SKIP`, followed by a separator. [GLLB:1726-1739] There is **no** click-to-cycle on the REBASE cell; no such handler exists in the source.

**Set-all split button**: "Pick/Squash/Edit ALL" applies to every row; "Skip/Squash/Edit unselected" applies to every row that is *not* selected. [RD:2975-3000, 363-372; GLLA:1320-1341]

**Validation rules**, applied by every setter [GLLA:1300-1341; RD:363-372]:

- Rows that are DONE or CURRENT cannot be changed.
- **Squash is refused on the bottom row** (there is nothing below to squash into).
- **Squash is refused on merge commits**, except in cherry-pick mode.

**Changing the action during a conflict or error**: setting the current commit to **Skip** immediately runs `git reset --hard` and continues. [RD:2955-2972]

Every setter posts `m_RebaseActionMessage` to the dialog.

**Other context-menu items in the Rebase list.** The Rebase dialog removes these: Cherry-pick, Switch to rev, Reset, Revert, Merge, Rebase-to-version, Revert-to-rev, Combine, Collapse/Expand. [RD:327-335] It enables Pick, Squash, Edit, Skip and **Show log**. [RD:1861-1865] Every other item from 1.11 that applies stays available: Compare with working tree, Compare with previous, Unified diff, Browse repository, Switch/Checkout to branch, Create branch/tag, Export, Edit notes, Format patch, Push, Delete ref, Copy to clipboard, Find, Show branches, and so on.

### 2.6 Reordering

- **Up / Down buttons** (and U / D): move every selected row one place up or down, keeping the selection. Nothing happens when the first selected row is already at the top (for Up) or the last selected row is already at the bottom (for Down). **Shift+click** moves the selected rows all the way to the top or bottom. [RD:2758-2837]
- **Drag and drop**: enabled only before the rebase starts, and only for a **contiguous** selection. [RD:1851, 1885; GLLB:3789-3801]
  - During the drag, a 4 px-high half-tone XOR insertion bar is drawn at the boundary above the target row, or below the last row.
  - The cursor shows "no-drop" when the target lies inside the selection or outside the list.
  - The list auto-scrolls when the pointer is near the top or bottom.
  - The drop posts `MSG_COMMITS_REORDERED(destIndex)`, and the block is moved with `std::rotate`. [GLLB:3803-3931; RD:2839-2870]
- **Add** opens a Log dialog in multi-select mode. The chosen commits are inserted as **Pick** at the **top** of the list, applied last. Add is disabled while Preserve merges is checked. [RD:3082-3122, 1850, 2719]

### 2.7 Lifecycle, button text and enabled states

The stages are: `Choose_Branch`, `Choose_Commit_Pick_Mode`, `Start`, `Continue`, `Abort`, `Finish`, `Conclict` (sic), `Edit`, `Squash_Edit`, `Squash_Conclict`, `Done`, `Error`. [RD.h:49-63]

**Continue button text** [RD:1797-1838; POT]:

| Stage | Text |
|---|---|
| Choose_* | "Start Rebase", or "Start (FastFwd)" when a fast-forward is possible |
| Start, Continue, Error, Squash_Conclict | "Continue" |
| Conclict | **"Commit"** |
| Edit | **"Amend"** |
| Squash_Edit | **"Commit"** |
| Abort, Finish | "Finish" |
| Done | "Done" |

**Enabled states** (`SetControlEnable` [RD:1840-1915]):

| Control | Choose_* (before start) | Any later stage |
|---|---|---|
| Branch / Upstream combos, Swap, Force, Preserve | enabled (not in cherry-pick mode) | disabled |
| Set-all, Up, Down, drag and drop | enabled | disabled |
| Add | enabled unless Preserve merges | disabled |
| Onto, "..." | as initialised | disabled |
| Edit/Split commit checkbox | hidden | visible and enabled only in Edit and Squash_Edit |
| Continue | enabled (unless the list state forbids it: no branch, equal refs, up to date, empty) | **disabled while the worker thread runs**; enabled otherwise, except in **Error** |
| Abort | always enabled. While the thread runs it asks "Are you sure you want to abort the rebase process?" and stops at the next step. | |
| Status text / Post button | status text | at Done with post entries: the status text is hidden and the post button is shown |

**Flow**

1. **Start.** Continue is pressed in a Choose stage.
   - It checks for a concurrent TortoiseGit rebase and for a clean worktree. If the worktree is dirty it offers "Stash" or "Abort" (stashing automatically if `rebase.autostash` is set), then checks the user name and email and runs the pre-rebase hook.
   - Tab 0 is renamed "Conflict Files", the Log tab is added, and the worker thread starts. [RD:1244-1339, 946-996]
   - `StartRebase` records `ORIG_HEAD` and the original branch, upstream and HEAD hashes, then runs `git checkout -f <upstream-or-onto>`. The log shows "Start Rebase". [RD:1035-1094]
2. **Continue loop** (worker thread) [RD:2399-2447]. For each row from the bottom up, the progress shows "Rebasing... (i/n)". The previous row loses CURRENT, the current row gains CURRENT (bold) and is scrolled into view. [RD:1917-1972] `DoRebase` [RD:2036-2389]:
   - **Skip**: marked done, no git command.
   - **Pick**: `git cherry-pick [--ff | -x] [--allow-empty] <hash>`. `-x` is used when "add cherry picked from" is checked; `--ff` is used in rebase mode for commits that are not part of a squash.
   - **Squash**, or a commit followed by a squash: `cherry-pick --no-commit`, accumulating the combined message.
   - **Edit**: the cherry-pick runs and then the loop **stops** in the Edit stage.
   - A failed pick that has **conflicts** stops in **Conclict** (or **Squash_Conclict**).
   - A failed pick **without conflicts** asks: "Cherry-pick failed ... Skip this commit?" with buttons Skip / Retry / Cancel and a "Do the same for the rest" checkbox. [POT:2480, 531]
   - An empty commit asks: "The current commit will be empty ... Skip the commit or keep the message only commit?" with buttons Commit / Skip / Cancel. [POT:9273]
3. **Conflict stage.**
   - The Conflict Files tab is activated and the file list shows unmerged, modified, added, deleted and renamed files, with checkboxes.
   - The message tab becomes editable, pre-filled with the commit message (cherry-pick mode uses `MERGE_MSG`).
   - The taskbar progress turns red.
   - If submodules changed, the status text turns into the red warning "..., take care of submodule changes". [RD:2449-2496, 2521-2545]
   - Pressing **Commit** refuses while files are still unmerged ("One or more files are in a conflicted state." and the first conflicted file is selected) or while the message still has conflict hints. Otherwise it stages the checked files, unstages the unchecked ones, runs `git commit -C <orig>`, amends the message if it was edited, and continues. [RD:1370-1566]
4. **Edit stage** (taskbar paused, Commit Message tab active and editable):
   - **Amend** runs `git commit --amend -F msg` and continues.
   - If the working tree is dirty, or **Edit/Split commit** is checked, a Commit dialog opens. In split mode it asks "Add another commit?" after each commit. [RD:1568-1729, 2546-2564]
5. **Squash_Edit stage.** The combined message is shown editable; **Commit** creates the squashed commit. The author and date come from the first commit; the date can follow the setting `SquashDate`: 0 = first commit, 1 = last commit, 2 = now. [RD:2565-2572, 1668-1681, 50]
6. **Finish.** `git checkout -f -B <branch> <newhead>` and `git reset --hard <newhead>`. Notes are rewritten, and the reflog gets "rebase: finished". Tabs are pruned to "Log", the status shows "Finished rebasing.", and the button reads **Done**. [RD:1129-1191]
7. **Done.** Pressing Done closes the dialog with OK and offers to pop the stash if one was created. The post button shows "Show log" and "Restart rebase"; choosing one closes the dialog with the code `IDC_REBASE_POST_BUTTON + index`. [RD:1235-1242, 2723-2731]
8. **Abort.** Esc and the window close button both route here.
   - Before the start, the dialog simply closes.
   - Afterwards it asks for confirmation, then restores the state: fast-forward → `reset --hard origBranch`; cherry-pick → `reset --hard origUpstream`; otherwise it checks out or resets the original branch to its original hash and restores the moved branch with `git branch -f`.
   - It writes the reflog entry "rebase: aborted", cleans up `.git/tgitrebase.active`, and offers to pop the stash. [RD:2579-2700]
9. **Fast-forward case.** "Start (FastFwd)" runs `checkout --no-track -f -B <branch> <upstream>` (for a local branch) and `reset --hard <upstream>`. The log shows `Fast forward to "X"`, and the dialog goes straight to Done. [RD:1253-1328]

Every failing git command in these steps shows a dialog with Retry / Abort buttons (`RunGitCmdRetryOrAbort`). [RD:3045-3063]

### 2.8 Options

- **Preserve merges** (default off): includes merge commits in the list; drops commits already in upstream (`rev-list --left-right --cherry-pick`); recreates merges with `merge --no-ff`. Add is disabled. The manual notes a reordering limitation. [RD:604-686, 2137-2261; MAN-REB]
- **Force Rebase** (default off; also set by the command-line `/force`): normally, when the branch already contains the upstream, the list shows "up to date" and Continue is disabled. With Force, the list of commits is shown anyway. [RD:583-597]
- **Onto** (toggle button): picks a ref through Browse References and becomes `--onto`. The ref is rejected when it equals the upstream or the branch. The button's checked state shows whether onto is set. [RD:3015-3038]
- **Swap ⇄**: exchanges Branch and Upstream, then reloads. [RD:2702-2708; MAN-REB]
- **"..."**: picks the upstream from Browse References. [RD:2710-2714]
- **add "cherry picked from"** (cherry-pick mode only): adds `-x`; persisted as `CherrypickAddCherryPickedFrom`. [RD:307, 2948-2953]
- **Cherry-pick mode differences**:
  - The Branch, Upstream, Swap, "...", Onto, Force and Preserve controls are hidden or disabled, and Upstream is fixed to "HEAD".
  - The commit list is supplied by the caller.
  - Merge commits ask "Which parent do you want to pick?" with buttons Parent 1 / Parent 2 / Cancel. [RD:293-313, 2115-2132]

### 2.9 Selection in the Rebase list

Before the rebase starts, selecting exactly one row fills tab 0 with that commit's changed files and tab 1 with its message. After the start, selection does not change the tabs. [RD:2879-2946]

---

## 3. Notes for implementers

1. **One list widget, three configurations.** Log, Rebase and RefLog all use `CGitLogListBase`, configured by flags: `m_IsRebaseReplaceGraph` (swaps the Graph column for REBASE), `m_IsIDReplaceAction` (swaps Actions for ID and makes SHA-1 visible by default), `m_bNoHightlightHead`, a 64-bit **context-menu mask** (one bit per command ID, below 64), and a per-list column registry key. Build tortilla's list the same way rather than writing three widgets. [GLLB.h:258-274, 323-419]
2. **The context menu is computed, not declared.** Every item depends on the selection count, WC row, merge commit, stash, HEAD, merge or bisect state, bare repository, the Shift key, and **which ref label was under the mouse**. Recording label rectangles while painting (`m_RefLabelPosMap`) is what makes the per-label menus possible. Its cache is cleared whenever the list scrolls (`GetTopIndex` changes). [GLLB:1157-1162]
3. **Command IDs with sub-indices.** Submenu entries encode a parent or branch index as `ID + (i << 16)`; the handler splits it apart again. Special values `0xFFFF`, `0xFFFE` and `0xFFFD` mean "all parents", "only merged files" and "extra changes after merge". [GLLB:1780, 1798-1805, 2020]
4. **Asynchronous diff stats.** The list loads hashes first (on a thread, `git log` through libgit). File lists and action icons are computed lazily on a second thread, only for the visible and selected rows, and cached on disk. The Actions column shows a "fetching" icon until the result arrives. Copy this; diffing every commit up front would be slow. [GLLB:124-204, 3147-3180; GLLB.h:640-663]
5. **The Working-tree row** is a fake revision with an empty hash and parent HEAD. Almost every code path tests `m_CommitHash.IsEmpty()`, so keep a single "is WC" predicate.
6. **The graph is hidden whenever filtering**, because lanes computed on a filtered list would be wrong. The compressed and labelled-only modes reuse the same filter flags (`FILTERSHOW_REFS`, `MERGEPOINTS`, `ANYCOMMIT`). [GLLB:1135-1136, 3182-3252]
7. **Ref label order is plain alphabetical order by full ref name**, except that a local branch's tracking remote is pulled forward to sit right after it and also drawn rounded. [GIT:2223-2226; GLLB:1208-1253]
8. **Dark-label detection**: `R*30+G*59+B*11 <= 12800` selects white text. Reuse it for user-configured colours. [GLLB:608]
9. **Clicking a column header opens the ordering dialog** (topo, date, author-date, reverse chronological) instead of sorting the list. [LD:2828-2848]
10. **Splitter clamping**: the middle (message) pane is always squeezed first, down to 20 px, before a neighbouring pane gives up space. [LD:2041-2042, 2074-2075]
11. **The filter is debounced** with a 1 s timer in both search boxes; Enter bypasses the timer. Regex validity is checked as you type: an invalid regex simply does not start the timer. [LD:3175-3178]
12. **The rebase "squash" direction is fixed by list order**: oldest at the bottom, "squash with commit below", and the last row can never be squash. Each setter enforces this, so invalid states are impossible rather than reported later.
13. **The REBASE column text is computed by masking the action bits** (`& LOGACTIONS_REBASE_MODE_MASK`), so DONE and CURRENT never change the text, only the styling. [GLLB:1544]
14. **Rebase index runs downward.** `m_CurrentRebaseIndex` starts at `count` and `GoNext()` decrements it (`m_IsOldFirst` is false). The ID column, progress "(i/n)" and log lines ("Pick 3: <hash>") count from the bottom. [RD:337-340, 1788-1795, 1917-1945, 2087-2089]
15. **Esc handling quirk** in Rebase: Esc pressed inside a tab child is re-posted to the dialog itself so MFC does not "cancel" the child control. tortilla only needs Esc in any child to behave as Abort. [RD:893-911]
16. **The Rebase dialog writes `.git/tgitrebase.active/{head-name,onto}` during conflicts**, so other TortoiseGit commands (and the "is a rebase active" check) know a rebase is in progress. A Linux clone that shares a repository with Windows TortoiseGit should honour that marker. [RD:2456-2466, 106-118; `CAppUtils::IsTGitRebaseActive`]
17. **Known source quirks** to replicate or fix knowingly:
    - BisectSkip reads the BisectBad registry key [COL:51].
    - The manual says four action icons, but the code draws five.
    - `MeasureItem` returns 50 px but has no effect.
    - The Add button has `BS_ICON` but no icon is assigned in `OnInitDialog` (whether an icon appears at runtime could not be determined from the source).
    - The Force checkbox is anchored TOP_CENTER while the others are anchored TOP_LEFT or TOP_RIGHT, so the checkboxes drift apart when the dialog is widened. [RD:146-148]
18. **Not determinable from source**:
    - Exact icon artwork (only the `.ico` file names: `actionmodified.ico`, `jumpup.ico`, `logfilter.ico`, etc. in `src/Resources/`).
    - The exact dark-theme colour mapping (`CTheme::GetThemeColor`, not examined).
    - The layout of the Find ("Search log messages") dialog (not examined).
    - The Statistics dialog (out of scope).

---

## 4. Explorer context menu (shell extension)

The shell extension is one COM class, `CShellExt`, that implements both the classic menu (`IContextMenu3`) and the Windows 11 top-level menu (`IExplorerCommand`). [SE.h:71-79] Both are built by the same function, `CShellExt::QueryContextMenu`, walking the same static table `menuInfo[]` in MI. [CTX:830-1116; MI:27-258]

### 4.1 Registration

- **Classic menu.** The installer registers the handler (`GuidUncontrolled`) under `ContextMenuHandlers\TortoiseGit` for `Drive`, `Directory`, `Directory\Background`, `Folder`, `LibraryLocation`, `LibraryFolder\background`, `lnkfile`, `InternetShortcut` and `*`. [SF:50-58, 93-95]
- **Right-drag menu.** A separate class (`GuidDropHandler`) is registered under `DragDropHandlers\TortoiseGit` for `Directory` and `Folder`. [SF:53, 59]
- **Windows 11 menu.** A sparse package declares `windows.fileExplorerContextMenus` with verb `TortoiseGit` (CLSID `A4FEC38E-C9F5-489A-ADAB-28DA10F523A3`, served by `TortoiseGitStub.dll`) for item types `Directory`, `Directory\Background` and `*`. There is no `Drive` entry. [APPX:38-57] The package is registered by TortoiseGitProc (`RegisterWin11ContextMenuCommand`, only on Windows 11 and after OOBE has completed) and by the "Register context menu" button on the settings page. [`src/TortoiseProc/Commands/RegisterWin11ContextMenu.cpp`; RC:2302]

### 4.2 The menu table

Each row of `menuInfo[]` is `{command, menuID, iconID, menuTextID, menuDescID, first, second, third, fourth}`. [MI.h:124-139]

- `menuID` is one bit of the 64-bit `TGitContextMenuEntries` mask (4.6). Several rows can share a bit.
- `first`..`fourth` are four `{yes, no}` pairs of `ITEMIS_*` flags. A pair matches when every `yes` bit is set **and** every `no` bit is clear. A pair `{0,0}` never matches; a pair with only `no` bits matches when all of them are clear. The row is shown when **any** of the four pairs matches. [MI.h:131-134; CTX:2322-2340]
- `menuDescID` is the help text Explorer shows for the hovered item (`GetCommandString`, `GCS_HELPTEXT`); entries without one use "no description for this command is available". [CTX:1658-1697; RS:258]

**Command lines.** Every command runs `<dir of the shell DLL>\TortoiseGitProc.exe` with ` /command:<name> ...`, and ` /hwnd:<parent HWND printed with %p>` is appended at the end. The one exception is "Review/apply single patch", which runs TortoiseGitMerge.exe with no `/hwnd`. [CTX:1217, 1560-1562, 1647-1654] The process is started through the Explorer window's `IShellDispatch2::ShellExecute` (after `AllowSetForegroundWindow(ASFW_ANY)`) when a site is available; otherwise it is created directly. [CTX:2264-2320] The path arguments come in three forms:

- `/path:<sel>`: `/path:"<first selected item>"`, or `/path:"<folder>"` when nothing is selected (`AddPathCommand` with files allowed). [CTX:1127-1136]
- `/path:<folder>`: always `/path:"<folder>"` (`AddPathCommand` with files not allowed).
- `/pathfile:<tmp>`: `/pathfile:"<temp file>" /deletepathfile`. The temp file is created in the TortoiseGit temp directory with prefix `git`. It holds the selected paths as raw UTF-16 with no BOM, each followed by `L"\n"`, or the folder alone when nothing is selected. The *folders only* variant skips selected items that are not directories. [CTX:1138-1146, 698-739]

Here *folder* (`folder_`) is the background folder, or the selected item itself when exactly one directory is selected. [CTX:355-358, 454-461] In the Windows 11 variant, *folder* is the first selected path. [EC:194-197]

**Placement column.** **Top** means the entry is in the default top-level mask; **Shift** means it is in the default extended mask (hidden unless Shift is held); a blank means the TortoiseGit submenu. "Win11" marks membership of the default Windows 11 mask. [SC.h:27-50]

**Flag abbreviations** in the conditions: ONE = ONLYONE, TWO, EXT = EXTENDED, GIT = INGIT, IVF = INVERSIONEDFOLDER, FOLD = FOLDER, FIG = FOLDERINGIT, ROOT = WCROOT, SUB = SUBMODULE, SUBC = SUBMODULECONTAINER, BARE = BAREREPO, MERGE = MERGEACTIVE, BISECT, STASH, SVN = GITSVN, CONFL = CONFLICTED, NORMAL, ADDED, DELETED, IGN = IGNORED, PATCH = PATCHFILE, INACC = INACCESSIBLE. `A&B !C` means "A and B set, C clear"; `|` separates the four alternatives. 4.3 explains how each flag is computed.

Rows in table order. `—` rows are the table's separators (see 4.5 for when they are actually emitted). Label strings are the exact resource strings; `&` marks the mnemonic.

| # | Label [RS line] | Icon (file) | `TortoiseGitProc` arguments | Conditions [MI line] | Placement |
|---|---|---|---|---|---|
| 1 | "Inaccessible..." [448] | `IDI_CLEANUP` (menucleanup.ico) | `inaccessible /path:<folder>` | INACC !GIT !FIG !BARE [29-30] | always top, also on Win11, ignores the extended mask [CTX:964, 1007-1011] |
| 2 | "Clone..." [421] | `IDI_CLONE` (menucheckout.ico) | `clone /path:<folder>` | FOLD !GIT !FIG !BARE !INACC \| FOLD&IGN \| FOLD&EXT [32-33] | Top, Win11 |
| 3 | "&Pull..." [419] | `IDI_PULL` (pull1.ico) | `pull /pathfile:<tmp>` (folders only) | FIG&ONE !BISECT !MERGE \| ROOT !BISECT !MERGE [35-36] | Win11 |
| 4 | "F&etch..." [281] | `IDI_UPDATE` (menuupdate.ico) | `fetch /pathfile:<tmp>` (folders only) | FIG&ONE \| BARE \| ROOT [38-39] | |
| 5 | "Pus&h..." [420] | `IDI_PUSH` (Push.ico) | `push /pathfile:<tmp>` (folders only) | FIG&ONE \| BARE \| ROOT [41-42] | Win11 |
| 6 | "Sync..." [297] | `IDI_RELOCATE` (menurelocate.ico) | `sync /path:<folder> /seq:<n>`, where n is a per-process counter starting at 0 [CTX:50, 1220-1228] | FIG&ONE !BISECT !MERGE [44-45] | Top, Win11 |
| — | | | | [48] | |
| 7 | "&Commit..." (label rewritten, see 4.4) [208] | `IDI_COMMIT` (menucommit.ico) | `commit /pathfile:<tmp>` | GIT \| FIG [50-51] | Top, Win11 |
| 8 | "SVN DCommit..." [295] | `IDI_COMMIT` | `svndcommit /path:<sel>` | GIT&SVN !BISECT !MERGE \| FIG&SVN !BISECT !MERGE [53-54] | |
| 9 | "SVN Rebase" [319] | `IDI_REBASE` (menurebase.ico) | `svnrebase /path:<folder>` | FIG&SVN&ONE !BISECT !MERGE [56-57] | |
| 10 | "SVN Fetch" [368] | `IDI_UPDATE` | `svnfetch /path:<folder>` | FIG&SVN&ONE [59-60] | |
| 11 | "Import SVN Ignore ..." [300] | `IDI_IGNORE` (menuignore.ico) | `svnignore /path:<folder>` | FIG&SVN&ONE [62-63] | Shift |
| — | | | | [65] | |
| 12 | "&Diff" [390] | `IDI_DIFF` (menucompare.ico) | `diff /path:"<p1>"`. With exactly 2 items: `/path:"<p1>" /path2:"<p2>"`; otherwise the folder. Shift held at click adds `/alternative`. [CTX:1332-1349] | GIT&ONE \| TWO !FOLD [67-68] | Win11 |
| 13 | "Diff later", or `Diff with "%s"` when a path is saved [381, 383] | `IDI_DIFF` | see 4.4 | ONE !FOLD [70-71] | |
| 14 | "&Diff with previous version" [412] | `IDI_DIFF` | `prevdiff /path:<sel>` [+ `/alternative` if Shift] | GIT&ONE !ADDED [73-74] | |
| — | | | | [76] | |
| 15 | "Show &log" [394] | `IDI_LOG` (menulog.ico) | `log /path:<sel>` | GIT&ONE !ADDED \| FOLD&FIG&ONE !ADDED \| FIG&ONE !ADDED \| BARE [78-79] | Win11 |
| 16 | "Show log of this folder" [409] | `IDI_LOG` | `log /path:<sel> /submodule` | FIG&ONE&ROOT&SUB [81-82] | |
| 17 | "Show &RefLog" [287] | `IDI_LOG` | `reflog /path:<folder>` | FIG&ONE \| BARE [84-85] | |
| 18 | "Browse References" [317] | `IDI_REPOBROWSE` (menurepobrowse.ico) | `refbrowse /path:<folder>` | FIG&ONE \| BARE [87-88] | |
| 19 | "Daemon" [414] | `IDI_DAEMON` (menudaemon.ico) | `daemon /path:<sel>` | same as Show log [90-91] | |
| 20 | "Revision &graph" [226] | `IDI_REVISIONGRAPH` (menurevisiongraph.ico) | `revisiongraph /path:<sel>` | same as Show log [93-94] | |
| 21 | "&Repo-browser" [377] | `IDI_REPOBROWSE` | `repobrowser /path:<folder>` | FIG&ONE \| BARE&ONE [96-97] | Win11 |
| 22 | "Check for modi&fications" [354] | `IDI_SHOWCHANGED` (menushowchanged.ico) | `repostatus /path:<sel>`, or `repostatus /pathfile:<tmp>` when more than one item is selected [CTX:1440-1445] | GIT \| FOLD&FIG [99-100] | |
| 23 | "Rebase..." [282] | `IDI_REBASE` | `rebase /path:<folder>` | FIG&ONE !BISECT !MERGE [102-103] | |
| 24 | "Stash changes" [283] | `IDI_SHELVE` (menushelve.ico) | `stashsave /path:<sel>` | GIT&ONE !MERGE [105-106] | Win11 |
| 25 | "Stash Apply" [284] | `IDI_UNSHELVE` (menuunshelve.ico) | `stashapply /path:<folder>` | FIG&ONE&STASH [107-108] | Shift |
| 26 | "Stash Pop" [299] | `IDI_UNSHELVE` | `stashpop /path:<folder>` | FIG&ONE&STASH [109-110] | |
| 27 | "Stash List" [289] | `IDI_LOG` | `reflog /path:<folder> /ref:refs/stash` | FIG&ONE&STASH [111-112] | |
| — | | | | [114] | |
| 28 | "Bisect start" [239] | `IDI_BISECT` (menubisect.ico) | `bisect /path:<folder> /start` | FIG&ONE !BISECT !MERGE [116-117] | |
| 29 | "Bisect good" [240] | `IDI_THUMB_UP` (thumb_up.ico) | `bisect /path:<folder> /good` | FIG&ONE&BISECT [119-120] | |
| 30 | "Bisect bad" [327] | `IDI_THUMB_DOWN` (thumb_down.ico) | `bisect /path:<folder> /bad` | FIG&ONE&BISECT [122-123] | |
| 31 | "Bisect skip" [336] | `IDI_BISECT` | `bisect /path:<folder> /skip` | FIG&ONE&BISECT [125-126] | |
| 32 | "Bisect reset" [335] | `IDI_BISECT_RESET` (menubisectreset.ico) | `bisect /path:<folder> /reset` | FIG&ONE&BISECT [128-129] | |
| — | | | | [131] | |
| 33 | "&Edit conflicts" [396] | `IDI_CONFLICT` (menuconflict.ico) | `conflicteditor /path:<sel>` [+ `/alternative` if Shift] | GIT&CONFL !FOLD [133-134] | Win11 |
| 34 | "Res&olve..." [213] | `IDI_RESOLVE` (menuresolve.ico) | `resolve /pathfile:<tmp>` | GIT&CONFL \| GIT&FOLD \| FIG [136-137] | |
| 35 | "Abort &Merge" [374] | `IDI_MERGEABORT` (menumergeabort.ico) | `merge /path:<sel> /abort` | GIT&MERGE \| FIG&MERGE [139-140] | Win11 (shares the Merge bit) |
| 36 | "Re&name..." [330] | `IDI_RENAME` (menurename.ico) | `rename /path:<sel>` [+ `/submodule` if SUB] | GIT&ONE&IVF !ROOT \| ROOT&SUB [142-143] | |
| 37 | "&Delete" [332] | `IDI_DELETE` (menudelete.ico) | `remove /pathfile:<tmp>` [+ `/submodule` if SUB] | GIT&IVF !ADDED !ROOT \| FIG&ROOT&SUB [145-146] | |
| 38 | "&Delete (keep local)" [427] | `IDI_DELETE` | `remove /pathfile:<tmp> /keep` | GIT&IVF !ADDED !ROOT [148-149] | |
| 39 | "Re&vert..." [211] | `IDI_REVERT` (menurevert.ico) | `revert /pathfile:<tmp>` | GIT !NORMAL \| FIG [151-152] | Win11 |
| 40 | "&Clean up..." [212] | `IDI_CLEANUP` | `cleanup /pathfile:<tmp>` | FIG&FOLD [154-155] | |
| — | | | | [157] | |
| 41 | "LFS" ► [342] | `IDI_LFS` (menulfs.ico) | submenu, see 4.4 | IVF&GIT [159-160] | |
| — | | | | [162] | |
| 42 | "S&witch/Checkout..." [231] | `IDI_SWITCH` (menuswitch.ico) | `switch /path:<folder>` | FIG&ONE [167-168] | Win11 |
| 43 | "&Merge..." [232] | `IDI_MERGE` (menumerge.ico) | `merge /path:<folder>` | FIG&ONE !BISECT !MERGE [170-171] | Win11 |
| 44 | "Create &Branch..." [233] | `IDI_COPY` (menucopy.ico) | `branch /path:<folder>` | FIG&ONE [173-174] | |
| 45 | "Create &Tag..." [278] | `IDI_TAG` (Tag.ico) | `tag /path:<folder>` | FIG&ONE [175-176] | |
| 46 | "E&xport..." [235] | `IDI_EXPORT` (menuexport.ico) | `export /path:<folder>` | FIG&ONE \| BARE [178-179] | |
| — | | | | [181] | |
| 47 | "Create repositor&y here..." [209] | `IDI_CREATEREPOS` (menucreaterepos.ico) | `repocreate /path:<folder>` | FOLD !GIT !FIG !BARE !INACC \| FOLD&IGN \| FOLD&EXT !GIT [183-184] | Top |
| 48 | "&Add..." [210] | `IDI_ADD` (menuadd.ico) | `add /pathfile:<tmp>` | IVF !GIT \| GIT&FOLD \| IGN \| DELETED !FOLD !ONE [186-187] | |
| 49 | "&Blame" [365] | `IDI_BLAME` (TortoiseGitBlame.ico) | `blame /path:<sel>` | GIT&ONE !FOLD !ADDED [189-190] | Win11 |
| 50 | "Add to &ignore list" ► [356] | `IDI_IGNORE` | submenu, see 4.4 | IVF !IGN !GIT !ROOT [192-193] | |
| 51 | "Delete and add to &ignore list" ► [220] | `IDI_IGNORE` | submenu, see 4.4 | IVF&GIT !IGN !ROOT [195-196] | (shares the Ignore bit with 50) |
| — | | | | [202] | |
| 52 | "Worktrees" [224] | `IDI_COPY` | `worktreelist /path:<folder>` | FIG&ONE \| BARE [204-205] | |
| 53 | "Submodule Add..." [285] | `IDI_ADD` | `subadd /path:<folder>` | FIG&ONE [207-208] | |
| 54 | "Submodule &Update..." [334] | `IDI_UPDATE` | `subupdate /pathfile:<tmp>`, plus `/bkpath:"<folder>"` when the folder flags contain SUBC, or the item flags contain SUBC&ROOT&ONE [CTX:1238-1246] | FIG&SUBC [210-211] | |
| 55 | "Submodule Sync" [286] | `IDI_MENUSYNC` (menusync.ico) | `subsync /pathfile:<tmp>` [+ `/bkpath` as above] | FIG&SUBC [213-214] | Shift |
| — | | | | [216] | |
| 56 | "Create Patch Serial..." [279] | `IDI_CREATEPATCH` (menudiff.ico) | `formatpatch /path:<folder>` | FIG&ONE [218-219] | |
| 57 | "Apply Patch Serial..." [280] | `IDI_PATCH` (menupatch.ico) | `importpatch /pathfile:<tmp>` | PATCH \| FIG&ONE [222-223] | |
| 58 | "Review/apply single &patch..." [369] | `IDI_PATCH` | **TortoiseGitMerge.exe**, see 4.4 | PATCH&ONE \| EXT&ONE !FOLD [226-227] | |
| 59 | "Send Mail..." [315] | `IDI_MENUSENDMAIL` (menusendmail.ico) | `sendmail /pathfile:<tmp>` | PATCH \| EXT !FOLD [229-230] | |
| — | | | | [237] | |
| 60 | "&Settings" [236] | `IDI_SETTINGS` (menusettings.ico) | `settings /path:<sel>` | FOLD \| !FOLD, i.e. always [239-240] | Win11 |
| 61 | "&Help" [400] | `IDI_HELP` (menuhelp.ico) | `help` (no path) | always [241-242] | |
| 62 | "A&bout" [237] | `IDI_ABOUT` (menuabout.ico) | `about` (no path) | always [243-244] | |

Icon files are from RS:29-87 and RS:106. The table's last five rows are the submenu pseudo-entries (`SubMenu*`, label "&TortoiseGit" [RS:238]), used only for their icon and text. [MI:246-257] Copy, Paste and Unignore rows are commented out. [MI:164-165, 198-200, 232-235]

### 4.3 How the `ITEMIS_*` flags are computed

The flags are computed in `CShellExt::Initialize` into two words: `itemStates` (the selection) and `itemStatesFolder` (the background or drop-target folder). For the normal menu, `itemStatesFolder` is OR-ed into `itemStates`. [CTX:52-503, 443-444] The bit values are in GLB:137-162.

**Selected items** [CTX:213-350]:

1. Each selected path is skipped if it is excluded by `NoContextPaths` (4.6) or lies inside a `.git` directory. Every remaining path is added to the selection list. Any item ending in `.diff` or `.patch` (case-insensitive) sets PATCH. [CTX:224-235]
2. Status is fetched for one item only: the first item for which the fetch succeeds (`statfetched`). Later items contribute only PATCH and their path. [CTX:236-237, 310]
3. **Status source.** When `CacheType` is `exe` (the default) and the path passes `IsPathAllowed` (the overlay include/exclude lists and drive-type masks), the status is asked from the TGitCache process (`GetStatusFromRemoteCache`). With the cache, an item outside any working tree is skipped without contributing flags. Otherwise `GitStatus::GetStatus` computes the status in-process. If that fails but the path is inside a working tree, the status is taken as `normal`. [CTX:250-309; SC:28, 352-419] With `SimpleContext` on, selected directories get no status call at all; a directory counts as `normal` when it is inside a working tree. [CTX:241-245]
4. Flags from the status call: FOLD when the item is a directory; FIG when it is a directory whose status is not unversioned, ignored or none; CONFL when conflicted. [CTX:266-273, 286-294]
5. `GetAdminDirMask()` (**filesystem only**) is OR-ed in [TGP:685-731]:
   - nothing if the path is not inside a working tree (no valid `.git` found walking up, see `GitAdminDir::HasAdminDir` [GAD:70-137]);
   - otherwise GIT and IVF;
   - for a directory, FIG; ROOT when the directory is the working-tree top; SUB when it is ROOT and its relative path matches a `submodule.*.path` in the parent repository's `.gitmodules` [TGP:733-754];
   - STASH when `<gitdir>/refs/stash` exists or `packed-refs` contains `refs/stash` [TGP:756-823];
   - SVN when `<gitdir>/svn/.metadata` exists;
   - BISECT when `BISECT_START` exists, and MERGE when `MERGE_HEAD` exists, both in the (worktree-specific) admin dir;
   - SUBC when `<worktree top>\.gitmodules` exists.

   FIG is therefore set for **every directory inside a working tree**, including unversioned ones. The source says so in a TODO, and nothing later clears FIG. [CTX:192-194]
6. **Status-derived adjustments**: GIT is cleared when the status is unversioned, ignored or none; IGN, NORMAL, CONFL, ADDED and DELETED are set from the matching status. [CTX:320-331]
7. After the loop:
   - IVF is set when the first item's **parent** directory is inside a working tree (`HasGITAdminDir(path, FALSE)`). [CTX:333-335]
   - If the first item is itself a bare repository (it contains `HEAD`, `config`, `objects\`, `refs\` and a directory `refs\heads`), `itemStates` is **replaced** by BARE alone. [CTX:337-338; GAD:355-383]
   - PATCHINCLIPBOARD is set when the clipboard holds format `TGIT_UNIFIEDDIFF`, and PATHINCLIPBOARD when it holds `CF_HDROP`. [CTX:342-349]

**Background folder** (`pIDFolder`) [CTX:355-450]:

- If the folder is excluded by `NoContextPaths`, it is dropped (`folder_` cleared).
- Otherwise its status is fetched the same way, unless it is the path already fetched. `itemStatesFolder` gets `GetAdminDirMask()`, NORMAL, CONFL, ADDED, DELETED, IGN and always FOLD. A bare repository replaces `itemStatesFolder` with BARE.
- INACC is set in `itemStates` when neither BARE nor IVF is set and either `<folder>\.git` exists, or bare detection found `refs\heads` that is not a directory (reftable format). [CTX:419-424]
- If no items are selected, ONE is set. [CTX:441-442]
- Quirk: on unversioned, ignored or none status, line 408 clears GIT in `itemStates`, not in `itemStatesFolder`. [CTX:407-408]

**Count flags and the single-directory case** [CTX:452-500]:

- TWO is set when exactly two paths remain.
- With exactly one path, ONE is set. If that path is a directory, it also becomes *folder*, and a second status call is made **always in-process** (`GitStatus`, never the cache) unless it is the path already fetched. That call adds `GetAdminDirMask()`, clears GIT on unversioned, ignored or none, and sets IGN, FOLD, ADDED and DELETED. INACC is computed as above. [CTX:459-497]

**Shift.** EXT is set when the shell passes `CMF_EXTENDEDVERBS`. [CTX:881-882] The extended-menu settings page describes this as "only be visible if the shift key is pressed while opening the context menu". [RC:1765] HASDIFFLATER is set when a diff-later path is saved; no table row tests it. [CTX:884-886]

**Summary of what needs git status.** These come from the filesystem only:

- GIT (initially), IVF, FIG, ROOT, SUB, STASH, SVN, BISECT, MERGE and SUBC (`GetAdminDirMask`);
- BARE and INACC;
- ONE, TWO, PATCH, EXT and the clipboard flags.

These need the status: clearing GIT for unversioned or ignored items, IGN, NORMAL, CONFL, ADDED and DELETED; FOLD and FIG also come from the status branch, and the single-directory path adds FOLD without a status.

**Windows limit.** For multi-selections, Windows passes at most 16 items to `Initialize` before `QueryContextMenu`, so the menu is built from at most the first 16 items. [CTX:2112-2115]

### 4.4 Entries with special text or behaviour

**Commit label** [CTX:526-588]:

1. The path is *folder*, or else the first selected item. If that path is inside a submodule (`GetAdminDirMask` has SUB), the text becomes "&Commit submodule..." [RS:223].
2. If `.git/HEAD` reads `ref: refs/heads/<name>`, the trailing dots are removed and ` -> "<name>"...` is appended. `&` in the branch name is doubled (`EscapeAccellerators`, `src/Utils/StringUtils.cpp:116-120`). Names longer than 64 characters are cut to 64 and given "..." before the quote.
3. Results:
   - top-level, on branch `main`: `Git &Commit -> "main"...`;
   - in the submenu: `&Commit -> "main"...`;
   - submodule: `&Commit submodule -> "main"...`.
4. On a detached HEAD, `GetCurrentBranchFromFile` returns non-zero, so the plain label is kept. The 40-hex-digit shortening branch in CTX:548-560 is therefore unreachable. [GIT:921-977; `Git.h:431` default `fallback = false`]

**Top-level prefix.** Entries placed at top level get the prefix `"Git "`. Their `&` mnemonics are removed when `ShellMenuAccelerators` is 0 (default 1). Submenu entries keep their `&`. [CTX:511-524]

**Diff later** [CTX:590-603, 1350-1375]:

- The saved path lives in `HKCU\Software\TortoiseGit\DiffLater`. [SE:37]
- When a path is saved, the label becomes `Diff with "<path>"`, with the path compacted by `PathCompactPathEx` into a 40-character buffer (`cchMax` 39). This replaces the whole text, so a top-level "Diff later" loses its "Git " prefix.
- On click:
  - Ctrl held: clears the saved path and runs nothing.
  - One item selected and nothing saved: saves the item's path and runs nothing.
  - One item selected and a path saved: runs `diff /path:"<item>" /path2:<saved, escaped>` [+ `/alternative` if Shift], then clears the saved path.
  - Any other selection: does nothing.

**Review/apply single patch** (TortoiseGitMerge.exe) [CTX:1494-1562]:

- If the clipboard holds a `TGIT_UNIFIEDDIFF` patch and the selection is not a patch file, the clipboard patch is written to a temp file and used.
- With a patch: ` /diff:"<patch>"`, then `" /patchpath:"<folder>` if the folder flags have FIG, then ` /wc` if IVF.
- Otherwise: ` /patchpath:"<first item or folder>"`.

**LFS submenu** [CTX:1926-1977]:

- Added only when the repository has a `<gitdir>\lfs` directory (`HasLFS` [TGP:897-906]) and at least one child matches.
- Children, all running TortoiseGitProc:

  | Label | Icon | Command | Condition |
  |---|---|---|---|
  | "Show locked files" [RS:344] | `IDI_REPOBROWSE` | `lfslocks /pathfile` | FIG&ONE |
  | "Lock" [RS:346] | `IDI_LFSLOCK` | `lfslock /pathfile` | GIT&IVF !FOLD |
  | "Unlock" [RS:348] | `IDI_LFSUNLOCK` | `lfsunlock /pathfile` | GIT&IVF !FOLD |

  [CTX:1928-1932, 1628-1636]
- The children are not affected by any mask. The "LFS" item always carries its icon, even when icons are disabled. [CTX:1960-1962]

**Ignore submenu** [CTX:1979-2262]:

- The submenu label is "Add to &ignore list" for unversioned items and "Delete and add to &ignore list" for versioned (GIT) items. Its icon is `IDI_IGNORE`; the children have no icons.
- Children:

  | Selection | Name item | Mask item (only when the item is not a folder and its name contains `.`) |
  |---|---|---|
  | One unversioned item | the file name → `ignore /pathfile` | `*<.ext from last dot>` → `ignore /pathfile /onlymask` |
  | One versioned item | the file name → `ignore /pathfile /delete` | `*<.ext>` → `ignore /pathfile /delete /onlymask` |
  | Several unversioned items | "&Ignore %d items by name" [RS:380] | "Ignore %d items by &extension" [RS:418] |
  | Several versioned items | "Delete and &ignore %d items by name" [RS:309] | "Delete and ignore %d items by &extension" [RS:218-219] |

- With several items, the mask item is always present, and both items carry the same `/delete` and `/onlymask` arguments as the single-item case. At 16 or more items, the "...multiple items..." variants replace the count. [RS:443-447]
- The unignore branch depends on `ignoredprops`, which is never filled, so it never runs. Rows 50 and 51 also exclude IGN. [SE.h:99; MI:198]

**Inaccessible** opens TortoiseGitProc's `inaccessible` command. It reports the libgit2 error and, for dubious-ownership errors or a `.git` file, offers to add the path to `safe.directory`. [`src/TortoiseProc/Commands/InaccessibleCommand.cpp:25-80`]

### 4.5 Structure of the classic menu

**Early exits.** No menu is added when [CTX:830-921]:

- `CMF_DEFAULTONLY` is set, or the flags are not `CMF_NORMAL`, `CMF_EXPLORE` or `CMF_VERBSONLY`;
- nothing is selected and there is no folder;
- the folder is one of the known special folders: Recycle Bin, CD burning, Favorites, Start Menu (common and user), Network, Connections, Control Panel, Cookies, Fonts, History, Internet, Internet Cache, NetHood, Printers, PrintHood, Recent, SendTo [CTX:1889-1924];
- any path starts with `::{` (a namespace item);
- `HideMenusForUnversionedItems` is on, Shift is not held, and none of GIT, IVF, FIG, BARE or INACC is set;
- the folder is inside `.git`;
- for a shortcut (`CMF_VERBSONLY`) with one item, `GitAdminDir::HasAdminDir(<the .lnk path>)` is false;
- the menu already contains an item whose `dwItemData` is the TortoiseGit marker string (to avoid a duplicate when several registrations fire).

**Build order** [CTX:938-1116]:

1. A separator is inserted at the shell's insert position.
2. The table is walked in order. For each non-separator row:
   - Skip if the row's bit is in the **extended mask** and EXT is not set (row 1 is exempt).
   - Skip if every bit of its `menuID` is in the **disable mask**.
   - Skip if no condition pair matches.
   - If the row's bit is in the **top-level mask**, insert it directly into the Explorer menu at the next top-level position, with the "Git " prefix. Otherwise append it to the TortoiseGit submenu.
3. After the walk, the submenu item is inserted below all top-level entries, followed by a separator. So the final layout is: separator, top-level entries in table order, "&TortoiseGit" ►, separator.
4. If the submenu is empty, it is not inserted. Only the leading separator, the top-level entries and a closing separator remain. [CTX:1036-1050]

**Submenu separators.** A separator row in the table only arms a flag. A separator is emitted just before the next submenu entry, and only if a submenu entry was added since the last emitted separator. So separators appear only between two groups that both contributed submenu entries; there is never a leading or trailing one. Top-level entries do not count toward this. [CTX:947-958, 975-985, 1025-1030] The separator test runs before the LFS and Ignore rows are expanded. A separator is therefore emitted when their table condition matches, even if the submenu then comes out empty (no `<gitdir>\lfs`, no selected files). The next group then emits none of its own, so only one separator results. These two rows set "entry added" but not "submenu non-empty", so a submenu holding only them would not be attached. [CTX:961-1001]

**Submenu item** [CTX:1055-1107]:

- Label "&TortoiseGit" [RS:238], with `&` stripped when `ShellMenuAccelerators` is 0.
- Icon, chosen by the selection:

  | Selection | Icon |
  |---|---|
  | A folder (background or one selected directory) | `IDI_MENUFOLDER` (tsvnmenufolder.ico); also carries the duplicate-detection marker |
  | One file | `IDI_MENUFILE` (tsvnmenufile.ico) |
  | One shortcut | `IDI_MENULINK` (tsvnmenulink.ico) |
  | Several items | `IDI_MENUMULTIPLE` (tsvnmenumultiple.ico) |
  | Otherwise | `IDI_APP` (Tortoise.ico) |

**Icon rendering.** Icons are drawn as 32-bit PARGB bitmaps (`MIIM_BITMAP`), and the menu gets style `MNS_CHECKORBMP`. `ShowContextMenuIcons` (default TRUE) turns them off. [CTX:605-617, 943, 1118-1125]

**What changes per situation** (default settings; derived from 4.2 and 4.3):

- **Right-click on a folder inside a working tree** (one item), and **on the background of such a folder**: both produce the same flags (FOLD, FIG, GIT, IVF, ONE, plus ROOT at the top), so they get the same menu. Top level: "Git Sync..." and `Git &Commit -> "<branch>"...`. For a clean repository root with no stash, submodules, LFS, SVN, bisect or merge in progress, the submenu holds these groups:

  | Group | Entries |
  |---|---|
  | 1 | Pull, Fetch, Push |
  | 2 | Diff, Diff with previous version |
  | 3 | Show log, Show RefLog, Browse References, Daemon, Revision graph, Repo-browser, Check for modifications, Rebase..., Stash changes |
  | 4 | Bisect start |
  | 5 | Resolve..., Revert..., Clean up... |
  | 6 | Switch/Checkout..., Merge..., Create Branch..., Create Tag..., Export... |
  | 7 | Add... |
  | 8 | Worktrees, Submodule Add... |
  | 9 | Create Patch Serial..., Apply Patch Serial... |
  | 10 | Settings, Help, About |

  On a subfolder (not ROOT), Rename..., Delete, Delete (keep local) and the ignore submenu become possible. Pull, Fetch and Push still appear through FIG&ONE.
- **Folder or background outside any repository**: the top level holds "Git Clone..." and "Git Create repositor&y here...". The submenu holds only Settings, Help and About.
- **File outside any repository**: no top-level entries. The submenu holds Diff later, Settings, Help and About. A `.diff` or `.patch` file adds Apply Patch Serial..., Review/apply single patch... and Send Mail....
- **File inside a repository** (tracked): the top level holds the Commit entry. Diff, Diff later, Diff with previous version, Show log, Daemon, Revision graph, Check for modifications, Stash changes, Rename, Delete, Delete (keep local), Revert (unless NORMAL), Blame and the ignore submenu follow from their conditions. Unversioned files lose GIT, so they get no Commit and no other GIT-only entries; they get Add... and "Add to ignore list" instead. Pull, Fetch, Push, Switch and the other FIG&ONE entries do not appear, because a file never has FIG.
- **Drive root**: there is no drive-specific code in CTX, so a drive is handled as a selected directory. The drive-type masks only choose between cache and in-process status (4.6).
- **Shift held** (EXT):
  - Import SVN Ignore, Stash Apply and Submodule Sync are no longer suppressed.
  - Clone appears for any folder, even inside a repository.
  - Create repository here appears for any folder that is not GIT.
  - Review/apply single patch appears for any single non-folder item.
  - Send Mail appears for any non-folder selection.
  - `HideMenusForUnversionedItems` is bypassed.
  - Shift **at click time** adds `/alternative` to Diff, Diff with previous version, Diff later and Edit conflicts. [CTX:1347-1348, 1368-1369, 1378-1379, 1422-1423]
- **Multi-selection**: ONE is cleared, so every ONE-conditioned row drops out. What remains is driven by the first item's flags: Commit, Check for modifications, Resolve, Revert, Delete, Delete (keep local), Add, the multi-item ignore labels, Settings, Help and About. Pull, Fetch and Push remain via ROOT when the first item is a repository root. Diff appears when exactly two non-folder items are selected (TWO !FOLD) and diffs them against each other. The submenu icon is `IDI_MENUMULTIPLE`.

**Keyboard and verbs.** In the submenu, `WM_MENUCHAR` is handled manually. One match executes the item; several matches cycle the selection. [CTX:1775-1843] Each inserted item also registers a canonical verb, namely its label with the first `&` removed (prefixed "Git " at top level), so `InvokeCommand` can be called by verb. [CTX:620-636, 1173-1181]

### 4.6 Settings that affect the menu

All values are under `HKCU\Software\TortoiseGit` unless stated. Changes are picked up through a registry-change notification on that key. [SC:26-171]

| Value | Default | Effect |
|---|---|---|
| `ContextMenuEntries` / `ContextMenuEntrieshigh` (DWORD low and high halves of the 64-bit mask) | `0x00000406` / `0x00000020` = Sync \| Commit \| CreateRepo \| Clone [SC.h:43-46; SC:48-50] | Bits set = entry on the **top level**, otherwise in the submenu. UI: "Context Menu" page, "Show the selected items on the first level of the context menu", with Restore Default. [RC:701-716; SLF:136-288] |
| `ContextMenuExtEntriesLow` / `ContextMenuExtEntriesHigh` | `0x40000000` / `0x00012000` = SVNIgnore \| StashApply \| SubmoduleSync [SC.h:48-50; SC:52-54] | Bits set = entry hidden unless Shift is held. UI: "Context Menu 2" page. [RC:1759-1769; SLF:298-376] |
| `ContextMenu11Entries` (QWORD) | `0x2000103801871096` = Sync \| Commit \| Switch \| Revert \| Conflicteditor \| Diff \| Pull \| Push \| RepoBrowser \| Log \| Blame \| Clone \| Merge \| StashSave \| Settings [SC.h:27-41; SC:46] | Bits set = entry shown in the Windows 11 top menu (4.7). UI: "Windows 11 Context Menu" page. [RC:2292-2303; SLF:418-489] |
| `ContextMenuEntriesMaskLow` / `High` in **HKLM and HKCU** (OR-ed together) | 0 / 0 [SC:56-59, 203-214] | Bits set = entry never shown (an administrator policy with no UI). The HKLM values are re-read at most every 10 s. [MAN-ADM:142-160] |
| `HideMenusForUnversionedItems` | FALSE [SC:41] | Suppresses the whole menu outside working trees unless Shift is held (4.5). UI: "Hide Menus for unversioned paths". [RC:711] |
| `NoContextPaths` (string, newline-separated) | "" [SC:69] | Paths excluded from the menu, case-insensitive [SC:311-350]: an entry ending in `*` is a prefix match; an entry ending in `\` matches that directory and everything below; any other entry is an exact match. Excluded items are dropped from the selection, and an excluded background folder is ignored. UI: "Do not show the context menu for the following paths:". [RC:712-713] |
| `ShellMenuAccelerators` | TRUE [SC:37] | When 0, `&` is stripped from top-level labels and the submenu name. |
| `ShowContextMenuIcons` | TRUE [CTX:943] | Icons on entries and on the submenu item. |
| `SimpleContext` | FALSE [SC:38] | Skips the status call for selected directories (4.3). |
| `EnableDragContextMenu` | TRUE [CTX:88, 743] | Enables the right-drag menu (4.8). UI: "Enable drag context menu". [RC:714] |
| `CacheType` | 1 (`exe`), or 2 (`dll`) in a remote-desktop session [SC:28; SC.h:72-78] | `exe` makes the menu ask TGitCache for status; any other value uses in-process `GitStatus`. |
| `OverlayIncludeList` / `OverlayExcludeList`, `DriveMaskFixed` (TRUE), `DriveMaskRemote`, `DriveMaskRemovable`, `DriveMaskCDROM`, `DriveMaskRAM`, `DriveMaskFloppy`, `DriveMaskUnknown` (all FALSE) | [SC:30-36, 690-695] | Feed `IsPathAllowed`. For the menu they only decide cache versus in-process status; they **do not** hide entries. Recycle-bin paths are always disallowed. [SC:717-735] |
| `DiffLater` | "" [SE:37] | The saved Diff-later path (4.4). |

**Bit-to-entry mapping** of the 64-bit mask [GLB:22-83]:

| Bit (hex) | Entries |
|---|---|
| `0x2` | Sync |
| `0x4` | Commit |
| `0x8` | Add |
| `0x10` | Revert |
| `0x20` | Cleanup |
| `0x40` | Resolve |
| `0x80` | Switch |
| `0x100` | Sendmail |
| `0x200` | Export |
| `0x400` | CreateRepo |
| `0x800` | Branch |
| `0x1000` | Merge (Merge... and Abort Merge) |
| `0x2000` | Remove |
| `0x4000` | Rename |
| `0x8000` | SubmoduleUpdate |
| `0x10000` | Diff |
| `0x20000` | Log |
| `0x40000` | Conflicteditor |
| `0x80000` | RefBrowser |
| `0x100000` | ShowChanged |
| `0x200000` | Ignore (both ignore submenus) |
| `0x400000` | RefLog |
| `0x800000` | Blame |
| `0x1000000` | RepoBrowser |
| `0x2000000` | ApplyPatch |
| `0x4000000` | RemoveKeep |
| `0x8000000` | SVNRebase |
| `0x10000000` | SVNDCommit |
| `0x40000000` | SVNIgnore |
| `0x100000000` | LogSubmodule |
| `0x200000000` | PrevDiff |
| `0x400000000` | ClipPaste (unused) |
| `0x800000000` | Pull |
| `0x1000000000` | Push |
| `0x2000000000` | Clone |
| `0x4000000000` | Tag |
| `0x8000000000` | FormatPatch |
| `0x10000000000` | ImportPatch |
| `0x20000000000` | DiffLater |
| `0x40000000000` | Fetch |
| `0x80000000000` | Rebase |
| `0x100000000000` | StashSave |
| `0x200000000000` | StashApply |
| `0x400000000000` | StashList |
| `0x800000000000` | SubmoduleAdd |
| `0x1000000000000` | SubmoduleSync |
| `0x2000000000000` | StashPop |
| `0x4000000000000` | DiffTwo (no table row) |
| `0x8000000000000` | Bisect (all five bisect rows) |
| `0x10000000000000` | Inaccessible |
| `0x80000000000000` | SVNFetch |
| `0x100000000000000` | RevisionGraph |
| `0x200000000000000` | Daemon |
| `0x400000000000000` | Worktree |
| `0x1000000000000000` | LFS |
| `0x2000000000000000` | Settings |
| `0x4000000000000000` | Help |
| `0x8000000000000000` | About |

Bit `0x1` is unused.

**Settings-page lists.** The settings pages list one row per distinct bit, in table order. They skip separators, the submenu pseudo-rows, Abort Merge and Inaccessible, and take the label with mnemonics removed. [SLF:30-59]

### 4.7 Windows 11 top-level menu (`IExplorerCommand`)

**Root command** [CTX:2342-2525]:

- Title "TortoiseGit" (literal, not localized); icon `TortoiseGitProc.exe,-<IDI_APP>`; flags `ECF_HASSUBCOMMANDS`; `Invoke` is not implemented.
- `GetState` hides it in these cases:
  - Inside the **classic** menu: the site exposes `IOleWindow` and the window class is not `NamespaceTreeControl`. In the navigation tree, the item is found by hit-testing the message position.
  - When `fOkToBeSlow` is false, it returns `E_PENDING` instead.
  - When no item can be resolved. For the folder background, the item is the folder shown in the Explorer view.
  - When `HideMenusForUnversionedItems` is on, Shift (`GetKeyState`) is not held, and none of GIT, IVF or FIG is set. Unlike the classic check, BARE and INACC do not count here.
  - When the built command list is empty.
- The selection is fed through the same `Initialize` (as a data object, so a background folder becomes a selected directory). Then `QueryContextMenu(nullptr, ..., CMF_EXTENDEDVERBS | CMF_NORMAL)` collects sub-commands instead of menu items.

**Differences from the classic menu**, all following from that call [CTX:881-882, 962-1032; EC:124-222]:

- **Different mask.** Only rows whose bit is in `ContextMenu11Entries` are added, plus Inaccessible (always). The result is one flat list; there is no nested TortoiseGit submenu below the root.
- **EXT is always set**, because the call passes `CMF_EXTENDEDVERBS`. Extended-only entries and EXT-conditioned entries behave as if Shift were held. With the default mask this means "Clone..." appears for every folder, including folders inside a repository.
- **Titles.** There is no "Git " prefix, and `&` is not stripped (the title string keeps the mnemonic). Exception: a submodule Commit whose bit is also in the classic top-level mask gets the title `Git &Commit submodule -> "<branch>"...`, because that branch prefixes "Git " whenever `istop` is set. [CTX:536-544]
- **Separators.** These are empty-title commands (`ECF_ISSEPARATOR`). The classic separator logic is reused, including its test of the classic top-level mask. A separator can therefore be emitted before a group whose entries are then not added because they are not in the Windows 11 mask. [CTX:975-985]
- **Icons** are `TortoiseGitProc.exe,-<iconId>`, or none when `ShowContextMenuIcons` is off.
- **LFS.** The LFS children (Show locked files, Lock, Unlock) are added **flat**, without the "LFS" parent, whenever the repository has LFS and the LFS row is not masked. This happens regardless of the Windows 11 mask, because `InsertLFSSubmenu` passes a null menu to `InsertGitMenu`, which then always appends a command. [CTX:997-1001, 1942-1953, 616-619]
- **Ignore.** Ignore entries are added only if the Ignore bit is in the Windows 11 mask. They are flattened after a separator, with each title prefixed by the submenu label and ": ". [CTX:989-996, 2236-2249]
- **Invoke** passes `GetForegroundWindow()` as hwnd and uses the first selected path as *folder*. [EC:173-199]
- **Diff later quirk:** `Invoke` reads and writes `HKCU\Software\TortoiseGitMerge\DiffLater`, while the classic menu uses `HKCU\Software\TortoiseGit\DiffLater`. The two menus do not share the saved path. [EC:193; SE:37]

**Default Windows 11 list.** For a clean repository-root folder, the list is, in table order:

1. Clone...
2. &Pull...
3. Pus&h...
4. Sync...
5. `&Commit -> "<branch>"...`
6. *separator*
7. &Diff
8. *separator*
9. Show &log
10. &Repo-browser
11. Stash changes
12. *separator* (emitted at the Bisect start row, which is then not added)
13. Re&vert...
14. *separator* (emitted at the LFS row)
15. S&witch/Checkout...
16. &Merge...
17. *separator* (emitted at the Add row)
18. &Settings

There is no separator between Sync and Commit. Commit is in the classic top-level mask, so it neither emits nor counts toward separators. This sequence was traced by hand through CTX:945-1033 with the default masks. Blame and Edit conflicts appear only for files that meet their conditions.

### 4.8 Right-drag menu and overlays (brief)

- **Right-drag** (drop handler, `QueryDropContext` [CTX:741-828]). With `EnableDragContextMenu` on, these flat entries are added when their source and target conditions hold, followed by a separator:
  - "Git Move versioned item(s) here" (`dropmove`)
  - "Git Move and rename versioned item here" (`dropmove /rename`)
  - "Git Copy and add files to this WC" (`dropcopyadd`)
  - for patch files, "Review/apply single &patch..." and "Apply Patch Serial..." (`importpatch`)
  - "Git New worktree..." (`dropnewworktree`)

  [RS:392-393, 426, 433] Drop commands pass `/pathfile` plus `/droptarget:"<folder>"`. [CTX:1148-1159]
- **Overlays and status cache.** Overlays are separate `IShellIconOverlayIdentifier` classes, one per state; `IsMemberOf` is the entry point. [IO:114-230; GLB:121-135] Overlays honour `IsPathAllowed` and use the same `CacheType` switch: `exe` queries TGitCache over its pipe, `dll`/`dllFull` use the in-process `GitFolderStatus` cache. The context menu shares only the `exe` cache path. Elsewhere it computes status in-process, and it never reads the overlay result. [CTX:250-309; IO:144-224]

### 4.9 Not established from source

- Whether Explorer passes a non-null `pIDFolder` for item selections (as opposed to background clicks). The code handles both, but *folder* for a plain file selection depends on it.
- How Explorer renders `&` in Windows 11 `IExplorerCommand` titles, and whether it collapses adjacent or dangling separators in either menu.
- Whether the Windows 11 `Directory` item type covers drive roots (the manifest has no `Drive` entry).
- Icon artwork (only the `.ico` file names are known).

---

## 5. Working-tree file lists: Check for modifications, folder Diff, Commit, and the shared `CGitStatusListCtrl`

This section is organised for building one shared component in tortilla. 5.2 is the file-list control that TortoiseGit embeds in many dialogs. 5.3 (Check for modifications) and 5.4 (Commit) describe only what those dialogs add on top of it. 5.5 lists the other dialogs that embed the same control. The Log dialog's use of the control (a commit's files rather than the working tree) is in 1.11.2.

"WC mode" below means the control shows working-tree changes, which is the case whenever `GetStatus` filled it: `UpdateFileList` empties `m_CurrentVersion` [GSLC:4133]. Many context-menu conditions test `m_CurrentVersion.IsEmpty()`.

### 5.1 Entry points: Explorer "Diff" on a folder, and `repostatus`

**Path arguments.** TortoiseGitProc reads either `/path` (one path, or several separated by `*`) or `/pathfile` (one path per line; the file is deleted when `/deletepathfile` is given). Passing both is an error. With no path at all, the list is the current directory. [TP:258-300] The repository is found from the first path, or else from the first list entry that is inside a repository. [TP:303-308] Each path is then made relative to the repository root (`Command::SetPaths`). The original first path is kept as `orgCmdLinePath`; the relative one is `cmdLinePath`. **The repository root itself becomes the empty path.** [CMDH:56-84]

**`diff` given one path** (`DiffCommand::Execute`, no `/path2`) [DIFFC:30-107]:

1. `CheckRepo(WorkingTreeOrBareRepoRequired)`. Outside a repository it shows "No valid git repository found." [POT:6481] and stops. [DIFFC:38-39; CMDC:464-471]
2. **If the original path is a directory and `/submodule` is not given, it opens the Check for modifications dialog (`CChangedDlg`, 5.3) modally, with a path list holding just that one (relative) path.** No diff tool is started. [DIFFC:40-46] This is the same window as `repostatus`. The only code difference is that `RepoStatusCommand` also sets `theApp.m_pMainWnd` to the dialog [RSC:24-31]. A folder Diff at the repository root therefore gets the empty path, which turns on the whole-project mode (5.3.4).
3. Otherwise (a file, or a directory with `/submodule`):
   - With `/startrev` and `/endrev`: diff the file between those two revisions, or show a unified diff with `/unified`. [DIFFC:52-58]
   - Without them: if libgit2 reports the file as `GIT_STATUS_INDEX_NEW` (staged as new), it first looks for a rename source with `git.exe diff-index --raw HEAD -M<n>% -C<n>% -z --`. If it finds one, it diffs the working file against the old path at `HEAD`. If not, it diffs against an empty file (`DiffNull`). [DIFFC:61-99]
   - Every other file is diffed **working tree against `HEAD`** (`CGitDiff::Diff(..., WorkingCopyRef, "HEAD")`), or shown as a unified diff with `/unified`. [DIFFC:101-104]
   - A directory with `/submodule` goes to the same `CGitDiff::Diff` call. For a directory, that call opens the submodule diff dialog instead of a file diff. [GD:387-399]
   - `/alternative` (sent by Explorer when Shift is held, 4.2 row 12) swaps the configured external and internal diff tools. [DIFFC:34; AU:478-487]

**`diff` given `/path2`.** Inside the repository and with `/startrev`+`/endrev`, the two paths are diffed at those revisions. Otherwise the two files are handed straight to the external diff tool (`StartExtDiff(path2, path)`). [DIFFC:108-124] Explorer sends `/path2` when exactly two non-folder items are selected. [4.2 row 12]

**Which diff tool.** `CAppUtils::StartExtDiff` uses the diff program configured for the file type (`PickDiffTool`). If none is configured, or the setting is commented out with `#`, it uses `TortoiseGitMerge.exe /base:%base /mine:%mine /basename:%bname /minename:%yname /basereflectedname:%bpath /minereflectedname:%ypath`. The alternative flag inverts that choice. [AU:466-500] `CGitDiff::Diff` resolves each revision with `^{}`, writes the non-working-tree side to a read-only temp file titled `<path>: <short hash>`, and titles the working-tree side "%s: Working Tree" [POT:263]. It then calls `StartExtDiff(file2, file1, ...)`, so **the older revision (rev2) is the left/base side and the working tree is the right/mine side**. [GD:361-484]

**`repostatus`** ("Check for modifications" in Explorer, 4.2 row 22) receives `/path:<sel>` for a single item, or `/pathfile:<tmp>` when more than one item is selected. [CTX:1440-1445] `RepoStatusCommand` keeps the default path requirement, *working tree required*. A bare repository or a path outside any repository shows "No working directory found." [POT:6485]. It then passes the whole relative path list to `CChangedDlg`. [RSC:24-31; CMDH:96; CMDC:456-462]

**Explorer menu visibility.** "Diff" is offered for a single item inside a repository, folders included (condition `GIT&ONE`), and for two non-folder items (`TWO !FOLD`). "Check for modifications" is offered for any item inside a repository (`GIT | FOLD&FIG`), including multi-selections. [4.2 rows 12 and 22]

### 5.2 The shared file list (`CGitStatusListCtrl`)

`CGitStatusListCtrl` is a report-mode `CListCtrl` with resizable columns (`CResizableColumnsListCtrl<CListCtrl>`) [GSLC.h:193-194]. The embedding dialog configures it with three calls:

- `Init(dwColumns, sColumnInfoContainer, dwContextMenus, bHasCheckboxes = true, bHasWC = true, allowedColumns = all)`, once [GSLC.h:438].
- `GetStatus(pathList, bUpdate, bShowIgnores, bShowUnRev, bShowLocalChangesIgnored, bShowLFSLocks, bGetStagingStatus)`, to load entries [GSLC.h:466; GSLC:370-376].
- `Show(dwShow, dwCheck, ...)`, to filter the loaded entries into rows and set the initial checkboxes [GSLC.h:479; GSLC:604].

The dialog also sets the empty-list text (`SetEmptyString`), and optionally a statistics label (`SetStatLabel`), a tri-state "select all" checkbox (`SetSelectButton`) and a confirm button (`SetConfirmButton`), which the control updates itself (5.2.7).

#### 5.2.1 `Init`

- **Extended style**: double-buffering, info tips and sub-item images, plus `LVS_EX_CHECKBOXES` when `bHasCheckboxes` is set. [GSLC:283-286] The base class then adds header drag-and-drop (columns can be reordered by dragging), and full-row select unless `FullRowSelect` is 0 (default 1). [`ResizableColumnsListCtrl.h:86-93`]
- **Font**: the UI font, or the log font when `LogFontForFileListCtrl` is set (default FALSE). [GSLC:289-294]
- **Icons**: the system small image list. Directories (submodules) use the folder icon; files use their shell icon. A "restore" overlay (`IDI_RESTOREOVL`) marks files flagged "Restore after commit". [GSLC:296-299, 1123-1133, 1169-1171]
- **Context-menu mask**: `dwContextMenus` is a 64-bit mask with one bit per command ID (`GetContextMenuBit(id) = 1 << id`). `GITSLC_POPALL` is all ones, and dialogs remove entries by XOR-ing named bits. [GSLC.h:112-139, 197-267]
- `bHasWC` = false hides working-tree-only entries: Compare with working tree, Commit, Revert, Blame, Explore to and the Shell submenu. [GSLC:1760, 1815, 1869, 1891, 2055]
- **File drops** are registered as an OLE drop target for `CF_HDROP`. Drops are accepted only after the dialog calls `EnableFileDrop()`, and never onto a group header or from the control's own drag. [GSLC:321-333, 3809-3813, 4486-4501]

#### 5.2.2 Loading (what git is asked)

`GetStatus` builds a load mask and calls `UpdateFileList(mask, bUpdate, pathList, bGetStagingStatus)` [GSLC:389-398]:

| Mask bit | Loaded by | Git command (per path in the list; without pathspec when the list is null or its first path is the root) |
|---|---|---|
| `FILELIST_MODIFY` (always) | `CGit::GetWorkingTreeChanges` | `git.exe diff-index --cached --raw HEAD --numstat -C -M -z --` (staged changes; **without a pathspec** whenever `m_bIncludedStaged` is set or there is no list), then `git.exe diff-index --raw HEAD --numstat -C<n>% -M<n>% -z -- <path>` (all changes against HEAD). `HEAD~1` replaces HEAD when the control's `m_amend` is set. Then `git.exe ls-files -u -t -z [-- <path>]` marks conflicts, and `git.exe diff --name-only --diff-filter=D -z [-- <path>]` adds deleted-but-unstaged files as Deleted+Missing. With Cygwin/MSYS2 git, a `git.exe status --no-ahead-behind -- <path>` runs first to refresh stat data. [GIT:3460-3653] In a repository without commits, `GetInitAddList` is used instead. [GIT:3462-3463] |
| `FILELIST_UNVER` (also loaded whenever `FILELIST_IGNORE` is requested) | `CTGitPathList::FillUnRev(UNVER)` | `git.exe ls-files --exclude-standard --full-name --others -z [-- <path>]`: individual files, not collapsed directories [TGP:1115-1168] |
| `FILELIST_IGNORE` | `FillUnRev(IGNORE)` | the same with `-i` (ignored files) [TGP:1130-1146] |
| `FILELIST_LOCALCHANGESIGNORED` | `FillBasedOnIndexFlags` | libgit2 index scan for entries with assume-valid (`GIT_INDEX_ENTRY_VALID`) or skip-worktree flags, filtered by path prefix [TGP:1218-1260] |
| `FILELIST_LOCKS` (only if `<gitdir>\lfs` exists) | `FillLFSLocks` | `git.exe lfs locks --json` [TGP:1171-1185] |

[GSLC:4307-4347]

- **Staging status** (`bGetStagingStatus`, used by the Commit dialog's staging mode): a file is *partially staged* when it appears in both `git.exe diff-index --cached --raw HEAD --numstat -C -M -z --` and `git.exe diff-files --raw --numstat -C -M -z --`. It is *totally staged* when it appears only in the first, and *totally unstaged* when only in the second. Conflicted files are always unstaged. [GIT:3527-3569]
- **Renames and copies** come from git's `-M`/`-C` detection; there is no separate rename pass. The similarity threshold is `ms_iSimilarityIndexThreshold`. [GIT:3507-3512]
- **Untracked submodules and directories**: `ls-files --others` without `--directory` lists files, so an untracked directory shows up as its files. [TGP:1135, 1144]
- **Missing files staged as Added or Modified**: for each such file, a message box asks "TortoiseGit detected that the file "%s" does not exist, but is staged as "Added" or "Modified". ..." [POT:9643]. The buttons are "R&estore this file from index" (`git.exe checkout -- <f>`), "&Remove this file from index" (`git.exe rm -f --cache -- <f>`) and "&Ignore", with a "&Do the same for the rest" checkbox. Afterwards an information box asks the user to refresh with F5. [GSLC:4142-4191]
- **Which entries start checked.** Every `FILELIST_MODIFY` entry starts with `m_Checked = TRUE`. Unversioned, ignored, local-changes-ignored and LFS-lock entries start unchecked. `Show()` then recomputes the check state (5.2.6). [GSLC:4145, 4220, 4270, 4288, 4301, 877]
- **Threading.** The control does not create a thread. The dialog calls `GetStatus` and `Show` from its own worker thread (5.3.5, 5.4.6), and the control guards its arrays with a reader/writer lock (`m_guard`). While `GetStatus` runs, `m_bBusy` is set and the list paints "Please wait..." [POT:7093]. [GSLC:378-383, 3698-3754]
- **Loaded-once flags.** `m_FileLoaded` records which lists are loaded. A later `UpdateFileList` call skips them unless `bUpdate`/`once` is true. `Clear()` resets the flags and also empties the changelists. [GSLC:4313-4344, 4349-4358]
- **Changelists** are read from `<worktree admin dir>\tgitchangelist` on every `GetStatus`. Lines `<name>` start a changelist; any other line is a repository-relative path. Paths listed before the first `<name>` line belong to `ignore-on-commit`. [GSLC:413, 4976-5018]

#### 5.2.3 `Show` and the show/check masks

`Show(dwShow, dwCheck, bShowFolders, UpdateStatusList, UseStoredCheckStatus)` clears the rows and rebuilds them. A loaded entry becomes a row when `entry->m_Action & dwShow` is non-zero. The `GITSLC_SHOW*` masks are mostly aliases of `CTGitPath::LOGACTIONS_*` bits. [GSLC:604-743; GSLC.h:58-110]

| Mask | Value |
|---|---|
| `GITSLC_SHOWUNVERSIONED` | `LOGACTIONS_UNVER` |
| `GITSLC_SHOWMODIFIED` | `LOGACTIONS_MODIFIED` |
| `GITSLC_SHOWADDED` | `LOGACTIONS_ADDED \| LOGACTIONS_COPY` |
| `GITSLC_SHOWREMOVED` | `LOGACTIONS_DELETED` |
| `GITSLC_SHOWCONFLICTED` | `LOGACTIONS_UNMERGED` |
| `GITSLC_SHOWREPLACED` | `LOGACTIONS_REPLACED` (renames) |
| `GITSLC_SHOWMERGED` | `LOGACTIONS_MERGED` |
| `GITSLC_SHOWIGNORED` | `LOGACTIONS_IGNORE` |
| `GITSLC_SHOWASSUMEVALID` / `GITSLC_SHOWSKIPWORKTREE` | `LOGACTIONS_ASSUMEVALID` / `LOGACTIONS_SKIPWORKTREE` |
| `GITSLC_SHOWDIRECTFILES` | `0x04000000`: pre-check files that were passed explicitly as paths (`m_setDirectFiles`) |
| `GITSLC_SHOWLFSLOCKS` | `0x00200000` |
| `GITSLC_SHOWFILES` / `GITSLC_SHOWSUBMODULES` | `0x01000000` / `0x02000000` (used only by `Check()`, 5.2.6) |
| `GITSLC_SHOWNORMAL`, `SHOWMISSING`, `SHOWOBSTRUCTED`, `SHOWEXTERNAL`, `SHOWINCOMPLETE`, `SHOWINEXTERNALS`, `SHOWREMOVEDANDPRESENT`, `SHOWDIRECTFOLDER`, `SHOWEXTERNALFROMDIFFERENTREPO`, `SHOWSWITCHED`, `SHOWINCHANGELIST` | **0**: SVN leftovers with no effect |
| `GITSLC_SHOWVERSIONEDBUTNORMALANDEXTERNALS` | MODIFIED\|ADDED\|REMOVED\|CONFLICTED\|REPLACED\|MERGED\|**IGNORED** (plus zero-valued bits) |
| `GITSLC_SHOWVERSIONEDBUTNORMALANDEXTERNALSFROMDIFFERENTREPOS` | the same set |
| `GITSLC_SHOWVERSIONED` | the same set (its NORMAL member is 0) |
| `GITSLC_SHOWALL` | `SHOWVERSIONED \| SHOWUNVERSIONED` |

- The common "versioned" masks include `SHOWIGNORED`. Ignored entries are kept out only because they are not loaded unless asked for.
- There is no "normal/unmodified" state: unmodified files are never loaded, and `GITSLC_SHOWNORMAL` is 0.
- **Too many items**: when `SetHideTooManyItems(true)` is in effect and more than `LogTooManyItemsThreshold` (default 1000) entries are loaded, no rows are shown and the list paints "Too many files to display." [POT:9617]. [GSLC:227, 646-661]
- **Sorting.** Initially there is no sort (`m_nSortedColumn = -1`), so rows appear in load order: changes as git printed them, then unversioned, ignored, local-changes-ignored and LFS locks. [GSLC.h:787; GSLC:666-670, 631-643] Clicking a header sorts by that column; clicking the same column again reverses the direction. A sort arrow (`HDF_SORTUP`/`HDF_SORTDOWN`) is drawn on the header. [GSLC:1235-1253, 720-734] The comparator works per column as follows [GSLCH:39-139]:
  - Path, Filename and Extension use `StrCmpLogicalW`, unless the Explorer policy `NoStrCmpLogical` is set, in which case `StrCmpI` is used.
  - Status compares the status text.
  - Lines added and Lines removed compare as numbers, and so do Last Modified and Size.
  - Ties fall back to the path.
  - The sort is stable.
- **Scroll position.** The dialog may call `StoreScrollPos()` before a reload. `Show()` then restores the first selected row, the selection mark and the scroll origin, unless `RememberFileListPosition` is 0 (default 1). [GSLC:899-928]

#### 5.2.4 Columns

Defined in `Init` [GSLC:302-319]. Each definition is `{id, header, visible by default = (dwColumns & bit), available = (allowedColumns & bit)}`.

| # | Header | Mask bit | Contents [GSLC:1018-1106] |
|---|---|---|---|
| 0 | "Path" [POT:6996] | always visible | Repository-relative path with `/`. For renames and copies with an old path: `new (from %s)` [POT:1330]. With `AbbreviateRenamings` (default FALSE), the abbreviated form `GetAbbreviatedRename()` is shown instead. |
| 1 | "Filename" [POT:4533] | `GITSLC_COLFILENAME` | file or directory name |
| 2 | "Extension" [POT:4296] | `GITSLC_COLEXT` | extension |
| 3 | "Status" [POT:8948] | `GITSLC_COLSTATUS` | The first matching name, in this order: "Conflict", "Added", "Missing", "Deleted", "Merged", "Modified", "Rename", "Copy", "Assume valid/unchanged", "Skip worktree", "Ignored". Anything else, unversioned files included, shows "Unknown". [TGP:1782-1811; POT:2899, 1620, 6196, 3454, 6144, 6208, 7676, 2956, 1869, 8764, 5524, 9914] |
| 4 | "Lines added" [POT:5898] | `GITSLC_COLADD` | numstat count, right-aligned |
| 5 | "Lines removed" [POT:5902] | `GITSLC_COLDEL` | numstat count, right-aligned |
| 6 | "Last Modified" [POT:5823] | `GITSLC_COLMODIFICATIONDATE` | File write time, as `FormatDateAndTime(..., DATE_SHORTDATE, true, RelativeTimes)`. Empty for deleted entries. |
| 7 | "Size" [POT:8747] | `GITSLC_COLSIZE` | `StrFormatByteSize64`; empty for directories; right-aligned |
| 8 | "LFS Lock Owner" [POT:5735] | `GITSLC_COLLFSLOCK` | lock owner; available only when the repository has `<gitdir>\lfs` |

- **Persistence.** Column order, widths and visibility are stored per container name under `HKCU\Software\TortoiseGit\StatusColumns\<container>`. The column version is 7. [GSLC:315-316; CM:113-162]
- **Header menu.** The header context menu is the column manager's menu: "Reset columns", then one checkable item per available column (as in 1.4). [CM:561-606]
- **Tooltip.** The info tip on a row is the full Windows path, unless `ShowListFullPathTooltip` is 0. [GSLC:3331-3350]

#### 5.2.5 Row colours and groups

**Text colours** are the same as the table in 1.11.2 [GSLC:3415-3440]. The colour comes from the first matching state, in this order: gray (unrelated path), conflict, modified, added/copied, deleted, renamed, merged, otherwise the window text colour. Unversioned, ignored, assume-valid and skip-worktree rows therefore use the normal text colour. The background is never changed.

**Groups** (`PrepareGroups`) [GSLC:3968-4119]:

- **When groups are on.** Group view is switched on when any of these holds: shown and loaded unversioned entries, shown and loaded ignored entries, shown and loaded local-changes-ignored entries, a multi-parent (merge) diff, or any changelist assignment.
- **Groups for the working tree**, in order:
  - 0 "Modified Files" [POT:6212]
  - 1 "Not Versioned Files" [POT:6509]
  - 2 "Ignored Files" [POT:5528]
  - 3 "Local changes ignored (assumed valid/unchanged or skip worktree flagged files)" [POT:5946]
  - 4 "(no changelist)" [POT:1335]
  - one group per changelist name (sorted, from `std::map`)
  - last, "ignore-on-commit"
- **Group of a row.** A row in a changelist goes to that changelist's group. Otherwise it goes to group 3 if flagged, group 2 if ignored, group 1 if unversioned, and group 0 for everything else. No row is ever assigned to "(no changelist)". [GSLC:1207-1233]
- **Merge diffs** (Log only) use "Merged Files" and "Diff with parent %d: <short hash>" instead. [GSLC:4003-4032]
- **Dark theme.** Group headers are custom-drawn in RGB(200,0,0) with a 2 px RGB(180,0,0) rule to the right of the label. [GSLC:3364-3396]

#### 5.2.6 Checkboxes (which dialogs, and what a check means)

- **Which dialogs.** Checkboxes exist only when `Init` was given `bHasCheckboxes` = true. These dialogs do: Commit, Revert, Add, Resolve and LFS locks. These do not: Check for modifications, Log, Rebase and the Sync lists. (Init calls in 5.5.)
- **Initial check state, per row**, in `Show` [GSLC:675-705]:
  1. In three-state (staging) mode, checked means totally or partially staged.
  2. Otherwise, a state remembered in `m_mapFilenameToChecked` for that path wins.
  3. Otherwise (unless `UseStoredCheckStatus`), the row is checked when:
     - its action bits intersect `dwCheck`, and it is not a missing file under `m_bNoAutoselectMissing`; or
     - `dwShow` has `SHOWDIRECTFILES` and the file was passed explicitly.

     Directories (submodules) are excluded from this when `m_bDoNotAutoselectSubmodules` is set, and files in the `ignore-on-commit` changelist are forced unchecked.
- **Clicking a box** checks or unchecks that row, and also every other selected row if the clicked row is selected. Each change is recorded in `m_mapFilenameToChecked`, so it survives a refresh. The parent then receives `GITSLNM_CHECKCHANGED` with the checked count. [GSLC:1255-1331, 1333-1464, 4121-4128]
- **Three-state mode** (`EnableThreeStateCheckboxes(true)`, Commit dialog staging mode) adds an indeterminate image `IDI_INDETERMINATE` for "partially staged". Clicking a checked box goes back to unchecked, never to indeterminate. Checking a row runs `git.exe add -- <path>` immediately. Unchecking runs the unstage command, which depends on the file's status [GSLC:931-983, 1282-1286, 1402-1406, 1459-1463]:
  - added: `git.exe rm -f --cached -- <path>`
  - deleted or modified: `git.exe reset -- <path>`
  - renamed: `git.exe rm -f --cached -- <new>` then `git.exe reset -- <old>`
- **Group header menu.** In a list with checkboxes and no selection, right-clicking a group header offers "Select changelist" [POT:8244] and "Deselect changelist" [POT:3510], which check or uncheck every row of that group. [GSLC:1575-1637, 1667-1672]
- **`Check(mask, check)`** checks or unchecks every row whose action bits, plus `SHOWFILES` or `SHOWSUBMODULES`, intersect the mask. It backs the Commit dialog's "Check:" links. [GSLC:3287-3329]
- **Meaning.** The meaning of a checked row is defined by the dialog (5.4.5 for Commit), not by the control.

#### 5.2.7 Counters, statistics text and placeholder text

- **Selected/total counter.** `GetStatisticsString()` writes "%1!d! files selected, %2!d! files total" [POT:148] (checked rows, row count) into the label set by `SetStatLabel`. It sets the `SetSelectButton` checkbox to unchecked, indeterminate or checked for none, some or all checked. It enables the `SetConfirmButton` button only when at least one row is checked. [GSLC:3213-3229]
- **Statistics text.** The return value is the statistics text. The full form is `IDS_STATUSLIST_STATUSLINE2`: "line: %d(+) %d(-) files: %s = %ld\n%s = %ld\n%s = %ld\n%s = %ld\n%s = %ld\n%s = %ld" [POT:10874]. Its parts are filled with:
  - lines added and removed (sums of numstat)
  - "normal" [POT:10955]: this count is never incremented, so it is always 0
  - "non-versioned" [POT:10947]: unversioned plus ignored
  - "modified" [POT:10919]: modified or renamed
  - "added" [POT:10726]
  - "deleted" [POT:10808]
  - "conflicted" [POT:10800]

  The simple form `IDS_STATUSLIST_STATUSLINE1` (lines plus modified/added/deleted/"replaced" [POT:11012]) is the one the Log dialog shows (1.10). The counts come from `BuildStatistics`, which walks **all loaded entries**, including loaded entries that the current filter hides. [GSLC:3181-3212, 1465-1510]
- **Placeholder text**, painted centred 10 px below the header [GSLC:3698-3754]:
  - while busy: "Please wait..." [POT:7093], or the dialog's busy string
  - with too many items: "Too many files to display." [POT:9617]
  - when empty: the dialog's empty string, defaulting to "File list is empty" [POT:4501]
- **Background image.** Dialogs may set a background image, a list-view watermark (`SetBackgroundImage`). [GSLC:365-368]

#### 5.2.8 Context menu (WC mode, full order)

The menu is built in `OnContextMenuList` [GSLC:1639-2063]. Section 1.11.2 lists the entries as they appear for a Log-dialog commit. In WC mode the order, conditions and labels are as below. Each item also needs its mask bit. "Sel" is the item under the selection mark, and its status decides most conditions. Shift held while opening adds the extra Add items. When the menu is opened from the keyboard, it is anchored at the centre of the selected row. [GSLC:1660-1666]

1. **Conflicted** (UNMERGED):
   - "&Edit conflicts" [POT:571], only for a single selection, and the default item.
   - "Resolved" [POT:7784].
   - Two items `Resolve conflict using "%s"` [POT:7779], filled with the theirs and mine titles from `GetConflictTitles`. The order is swapped while a rebase is active.
   - *separator*

   [GSLC:1691-1719]
2. **Unversioned or ignored**:
   - "Add" [POT:1563]
   - with Shift on a file: "Add as Executable (+x)" [POT:1588] and "Add as Symlink" [POT:1592]
   - "Commit..." [POT:2750]

   [GSLC:1721-1737]
3. **Versioned and not merely missing** [GSLC:1739-1780]:
   - "Compare with b&ase" [POT:2819]. It is the default item unless a single conflicted file is selected.
   - "Show changes as &unified diff" [POT:8516], not offered in a repository without commits.
   - *separator*

   "Compare with &working tree" needs a commit (`m_CurrentVersion`), so it never appears in WC mode.
4. **Exactly two non-directory rows selected**: "Compare two files" [POT:2807]. [GSLC:1796-1812]
5. **Versioned rows with a working tree** [GSLC:1815-1859]:
   - "Commit..." (not for skip-worktree or assume-valid rows)
   - "Re&vert..." [POT:7386]
   - "Skip worktree" [POT:8764] (not for added, conflicted or already skip-worktree rows)
   - "Assume Unchanged" [POT:1865] (not for added, deleted, conflicted or already assume-valid rows)
   - "Unflag as skip-worktree and assume-unchanged" [POT:9902] (only for flagged rows)
   - "Restore after commit" [POT:7809], or "Restore" [POT:7801] once a backup exists, for files only
   - the LFS lock and unlock items, when the repository has LFS and the row is not conflicted [GSLC:2776-2825]
6. **Single selection, not unversioned or ignored** [GSLC:1861-1871]:
   - "Show &log" [POT:8440]
   - "Show log of submodule" [POT:8609] (directories)
   - "Show log &before rename/copy" [POT:8596] (renamed or copied rows with an old path)
   - "&Blame" [POT:359] (files that are not deleted and not newly added)
7. **Any selection, not deleted or missing**: "Export selection to..." [POT:4275]. [GSLC:1873-1877]
8. **Single selection** [GSLC:1879-1915]:
   - "Save revision &to..." [POT:8137] (files that are not deleted)
   - For files that are not deleted or missing: "View revision in alternative editor" [POT:10318], "&Open" [POT:922] and "Open with..." [POT:6774]
   - "Explore to" [POT:4259] (not deleted)
   - *separator*, then "Mark for comparison" [POT:6075] (not deleted). Once a file is marked, or a path was saved by Explorer's "Diff later", it is followed by `Diff with "%s"` [POT:3614].
9. **Unversioned, ignored or missing**: "&Delete" [POT:479]. [GSLC:1927-1931]
10. **Unversioned or deleted**: the ignore items [GSLC:1932-1985]:
    - **All selected items share one extension**: an "Add to &ignore list" [POT:1608] submenu with the file name (or "&Ignore %d items by name" [POT:731] for several items), then `*<ext>`, then (single item in a subfolder) the containing folder's path.
    - **Mixed extensions**: flat items, "Add to &ignore list" or "&Ignore %d items by name", plus "Ignore %d items by &extension" [POT:5447].
11. *separator*, then "Copy to clipboard" [POT:2992] ►:
    - "full paths" [POT:10832]
    - "relative paths" [POT:11004]
    - "file/foldernames" [POT:10828]
    - "Copy all information to clipboard" [POT:2964] (all visible columns with a header line, tab-separated)
    - `column '%ls'` [POT:10791], for the column that was right-clicked

    [GSLC:1990-2008, 3880-3951]
12. **Changelists**. Shown unless the row's action bits equal the `git_wc_status_unversioned` or `git_wc_status_none` enum values. That test compares an action bitmask with an SVN-style enum.
    - *separator*
    - "Remove from changelist" [POT:7635], when a selected row is in one
    - "Move to changelist" [POT:6232] ►: "<new changelist>" [POT:1425], *separator*, "ignore-on-commit", then *separator* and the existing changelists
    - the checkable "Keep changelists" [POT:5707] (registry `KeepChangeLists`, default FALSE)

    [GSLC:2010-2052, 242-243]
13. **Not deleted or missing, with a working tree, in WC mode**: *separator*, then "Shell" [POT:8396] ►, the Windows shell context menu for the selected files. [GSLC:2054-2061]

**What the WC-mode commands do** [GSLC:2114-2760]:

| Command | Action |
|---|---|
| Edit conflicts | `CAppUtils::ConflictEdit` (Shift: alternative tool) |
| Resolved / Resolve using | Confirm "Are you sure you want to mark the conflicted file(s) as resolved?" [POT:1838], then `ResolveProgressCommand` in the progress dialog. The row becomes Modified. |
| Add | `AddProgressCommand` in the progress dialog. It offers a Commit button afterwards if the mask has Commit. Then the parent refreshes. |
| Commit... | `TortoiseGitProc /command:commit /pathfile:<tmp> /deletepathfile` with the selected paths |
| Compare with base / Enter / double-click | `StartDiff` (5.2.9) |
| Show changes as unified diff | For each selected file, the unified diff `HEAD[~1]` → working tree (`diff.context` honoured) is concatenated into one read-only temp file and opened in the unified diff viewer. Shift picks the alternative viewer. |
| Compare two files | `TortoiseGitProc /command:diff /path:A /startrev:<WC or HEAD if A is missing> /path2:B /endrev:<WC or HEAD> /hwnd:<list>`, plus `/alternative` with Shift |
| Revert... | If any selected row is a modified file, confirm "Are you sure you want to revert %d item(s)? You will lose ALL changes since the last update!" [POT:1842]. Then `RevertProgressCommand` to `HEAD` (`HEAD~1` when amending). Reverted rows are removed from the list; added files or directories trigger a full refresh. |
| Skip worktree / Assume Unchanged / Unflag | Confirm with [POT:3760] / [POT:3756] / [POT:3811], then set the index flags |
| Restore after commit / Restore | Copy the file to a temp file and show the overlay. Restore copies it back after confirmation [POT:3797]. |
| Show log | `TortoiseGitProc /command:log /path:<full path>` (+ `/submodule` for directories) |
| Blame | `CAppUtils::LaunchTortoiseBlame` |
| Explore to | `CAppUtils::ExploreTo` |
| Delete | Removes the paths from the index (libgit2). The existing files go through `SHFileOperation(FO_DELETE)`, to the recycle bin unless Shift is held. Rows are removed, or the list refreshes if the index changed. [GSLC:4702-4784] |
| Ignore items | `CAppUtils::IgnoreFile(list, byMask)`, then the parent refreshes |
| Move/Remove changelist | Edit `tgitchangelist` (`SaveChangelists`) |

**Many diffs at once.** Selecting more than `max(3, NumDiffWarning)` files (default 10) for a compare first asks "You have selected %d items to show the diff for. ..." [POT:10572]. [GSLC:4360-4370]

#### 5.2.9 Double-click, keyboard, drag and drop, parent notifications

**Double-click** (when `m_bEnableDblClick`, default true; ignored on the checkbox image) [GSLC:2926-2976]:

- Unversioned or ignored: `StartDiffWC`, i.e. `CGitDiff::Diff(file, file, WorkingCopyRef, m_CurrentVersion)`. In WC mode the second revision is the empty string (5.6).
- Conflicted: `ConflictEdit` (Shift: alternative). The list re-shows afterwards, or refreshes if the file no longer exists.
- Missing (and not also Deleted): nothing happens.
- Otherwise `StartDiff` (WC mode) [GSLC:3026-3060]:
  - In a repository without commits: `DiffNull(working file)`.
  - Added: `DiffNull(..., bAdd=true)`, the file against an empty one.
  - Deleted: `DiffNull("HEAD"[~1], bAdd=false)`, the HEAD version against nothing.
  - Anything else: **`CGitDiff::Diff(new path, old path for renames and copies, WorkingCopyRef, "HEAD")`**, with `HEAD~1` when amending a file that is not added. This is HEAD on the left and the working tree on the right, in the configured diff tool (5.1). Shift held uses the alternative tool.

**Keys** [GSLC:3832-3878, 3611-3690]:

| Key | Action |
|---|---|
| Enter | Same as double-click, for every selected row; the list takes Enter from the default button (`DLGC_WANTALLKEYS`) |
| Ctrl+A | Select all rows |
| Ctrl+C, Ctrl+Insert | Copy relative paths, one per line |
| Ctrl+Shift+C, Ctrl+Shift+Insert | Copy relative path and status, tab-separated, with a header line |
| Delete | Delete the selected files, if the mask has Delete and the row under the selection mark is unversioned or ignored |
| Tab / Shift+Tab | Move focus to the next or previous dialog control |
| Esc | Send `WM_CLOSE` to the dialog |

**Drag out.** Dragging rows starts an OLE drag (`DROPEFFECT_MOVE | DROPEFFECT_COPY`) of a `GitDataObject` holding the selected paths and the shown revision. [GSLC:3767-3807]

**Drop in.** Dropped files are only accepted after `EnableFileDrop()`. Each dropped path is sent to the parent as `GITSLNM_ADDFILE`. [GSLC:4373-4501]

**Notifications to the parent.** The control sends the parent these messages [GSLC.h:268-294]:

- `GITSLNM_NEEDSREFRESH`, after commands that change the working tree (`RefreshParent`)
- `GITSLNM_ITEMCOUNTCHANGED`
- `GITSLNM_ADDFILE`
- `GITSLNM_CHECKCHANGED` (wParam = checked count)
- `GITSLNM_ITEMCHANGED`, on any item change, including selection

### 5.3 Check for modifications (`CChangedDlg`, `IDD_CHANGEDFILES`)

#### 5.3.1 Window

- **Template.** `IDD_CHANGEDFILES DIALOGEX 0,0,520,243`, caption **"Working Tree"**, resizable (`WS_THICKFRAME`) with minimize and maximize boxes. Unlike the Log dialog it has no `WS_EX_APPWINDOW`. [RC:187-190]
- **Title.** `<path> - Working Tree - TortoiseGit` [CHD:247-261; AU:3518-3551, 3562-3565]. The path is:
  - the repository root in whole-project mode;
  - otherwise the single given path, made absolute;
  - with several paths, the common directory of the loaded entries.

  It is compacted with `PathCompactPathEx` to `80 − len("Working Tree")` characters. When `DialogTitles` is 1, the part above the repository's parent folder is stripped. The title is set when the dialog opens and again after every load.
- **Placement.** The dialog is centred on the Explorer window when one is known, and its size and position are saved and restored (`EnableSaveRestore(L"ChangedDlg")`). [CHD:129-131]

#### 5.3.2 Layout (top to bottom)

Controls from RC:191-208; anchors from CHD:113-127.

| Control | ID | Type | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|---|
| Branch link | `IDC_BRANCH` | SysLink, no prefix | `<a>current branch</a>` after loading; empty while loading [CHD:161, 186] | 7,2,506,9 | top, stretches |
| File list | `IDC_CHANGEDLIST` | `CGitStatusListCtrl`, report, show-selection-always, **no checkboxes** | – | 7,14,506,152 | stretches both ways |
| Unversioned | `IDC_SHOWUNVERSIONED` | auto checkbox | "Show un&versioned files" | 7,171,260,10 | bottom-left |
| Local changes ignored | `IDC_SHOWLOCALCHANGESIGNORED` | auto checkbox | "Show ignore local changes flagged files" (no mnemonic) | 7,181,263,10 | bottom-left |
| Ignored | `IDC_SHOWIGNORED` | auto checkbox | "Show i&gnored files" | 7,191,263,10 | bottom-left |
| Unmodified | `IDC_SHOWUNMODIFIED` | auto checkbox, **hidden** (`NOT WS_VISIBLE`) | "Show un&modified files" | 7,201,263,10 (same place as the next one) | bottom-left |
| Staged | `IDC_SHOWSTAGED` | auto checkbox | "Show all &staged files" | 7,201,263,10 | bottom-left |
| Whole project | `IDC_WHOLE_PROJECT` | auto checkbox | "Show &Whole Project" | 7,211,263,10 | bottom-left |
| Statistics | `IDC_INFOLABEL` | multi-line read-only edit, no border | see 5.3.6 | 294,169,219,23 | bottom-right |
| Summary | `IDC_SUMMARYTEXT` | static | empty; never written by the code (only anchored) [CHD:115] | 294,193,219,13 | bottom, stretches |
| Unified diff | `IDC_BUTTON_UNIFIEDDIFF` | push button | "Save unified diff" | 109,222,119,14 | bottom-right |
| Stash | `IDC_BUTTON_STASH` | `CMenuButton`, drop arrow always shown [CHD:134] | "Stash" | 236,222,68,14 | bottom-right |
| Commit | `IDC_COMMIT` | push button | "Commit" | 312,222,67,14 | bottom-right |
| Refresh | `IDC_REFRESH` | push button | "&Refresh" | 388,222,67,14 | bottom-right |
| OK | `IDOK` | default button | "OK"; "Cancel" while loading | 463,222,50,14 | bottom-right |

- **No Cancel button.** There is no Cancel or Help button. Esc and the close box go through `OnCancel`. [CHD:211-219]
- **Hidden mnemonic.** The hidden "Show un&modified files" checkbox still owns Alt+M; whether a hidden control's mnemonic fires could not be established from source.
- **Resizing.** Every checkbox is widened to fit its text (`AdjustControlSize`). [CHD:106-111]
- **Theme.** The list gets the background image `IDI_CFM_BKG` and the dialog follows the dark theme. [CHD:103, 132]

#### 5.3.3 Checkboxes: defaults, persistence, effect

| Checkbox | Default | Persisted | Effect [CHD:263-309, 450-462, 221-245] |
|---|---|---|---|
| Show unversioned files | `HKCU\Software\TortoiseGit\AddBeforeCommit` (default TRUE) [CHD:82-83] | yes, written on click; the same value drives the Commit dialog's "Show Unversioned Files" and the Log's View option | If unversioned files are already loaded, the list is re-filtered. Otherwise, when turned on, the dialog reloads. |
| Show ignore local changes flagged files | FALSE [CHD:36] | no | Adds assume-valid and skip-worktree entries (group 3); re-filters or reloads as above |
| Show ignored files | FALSE [CHD:35] | no | Adds ignored files (group 2); re-filters or reloads as above |
| Show unmodified files (hidden) | 0 [CHD:34] | no | Sets `GITSLC_SHOWNORMAL`, which is 0, so it has no effect. Its click handler also rewrites `AddBeforeCommit`. [CHD:280-287] |
| Show all staged files | `HKCU\Software\TortoiseGit\TortoiseProc\ChangedFilesIncludeStaged` (default TRUE) [CHD:89-90] | yes, on click | Sets `m_bIncludedStaged`. Staged changes are then listed **for the whole repository**, even outside the given paths (the staged `diff-index` runs without a pathspec). Reloads. [CHD:167; GIT:3505-3509] |
| Show Whole Project | `HKCU\Software\TortoiseGit\TortoiseProc\ShowWholeProject\<repo root with ':' replaced by '_'>` (default FALSE) [CHD:85-88] | yes, on click; per repository; **the same key as the Commit dialog's checkbox** [CD:218-221] | Loads with no path list (the whole repository) and puts the repository root in the title. Reloads. |

**Enabled states** [CHD:154-159, 182-193]:

- **While loading**: Refresh and every checkbox except Show Whole Project are disabled.
- **After loading**:
  - "Show unversioned files" and "Show ignored files" are enabled only when exactly one path was given and it is a directory or the repository root. With a single file or several paths they stay disabled, but they keep their value, which still decides what is loaded.
  - "Show ignore local changes flagged files" and "Show all staged files" are always enabled.
  - "Show Whole Project" is never disabled by the thread.

#### 5.3.4 Buttons and the branch link

- **OK** closes the dialog. While a load is running, the button reads "Cancel" [POT:2280]. Clicking it, or pressing Esc, sets the cancel flag instead of closing. [CHD:153, 187, 201-219]
- **&Refresh** and **F5**, in any control (`PreTranslateMessage`), start a reload. If one is already running, nothing happens. [CHD:330-355]
- **Commit** opens the Commit dialog through `CAppUtils::Commit` [CHD:365-385; AU:3062-3140]. The path is the single file, if exactly one file was given; otherwise it is the directory of the common root of the path list. The Commit dialog also receives the whole-project state, an empty message and `SelectFilesForCommit` (default TRUE). This dialog reloads when the Commit dialog closes.
- **Stash** shows a menu below the button, aligned left, excluding the button rectangle. Every entry reloads the list afterwards. [CHD:387-433]

  | Entry | When shown | Action |
  |---|---|---|
  | "Stash changes" [POT:8924] (`IDI_SHELVE`) | always | `CAppUtils::StashSave` |
  | "Stash Pop" [POT:8916] | when the repository has a stash (`HasStashDir`) | `CAppUtils::StashPop(hWnd, 2)` |
  | "Stash Apply" [POT:8884] | when the repository has a stash | `CAppUtils::StashApply(hWnd, "", false)` |
  | "Stash List" [POT:8900] | when the repository has a stash | RefLog dialog on `refs/stash` |

- **Save unified diff** runs `TortoiseGitProc /command:showcompare /unified /path:<p> /revision1:HEAD /revision2:<working-copy ref>`, plus `/alternative` when Shift is held. [CHD:435-448] `<p>` depends on the path list:
  - a single file: that file;
  - whole-project mode: the repository root (an empty relative path);
  - otherwise: the directory of the common root.

  `showcompare /unified` writes the unified diff to a read-only temp file and opens it in the unified diff viewer. **No file is saved by this button**; saving would happen in the viewer. [SCC:24-42; AU:913-935]
- **Branch link** (`IDC_BRANCH`): clicking it runs `CAppUtils::Switch` (the Switch/Checkout dialog) and reloads if that returns true. [CHD:323-328]

#### 5.3.5 Loading

Loading runs in `ChangedStatusThread` on a worker thread (`AfxBeginThread`); a re-entry flag blocks a second load. [CHD:150-199, 345-355] The thread does the following, in order:

1. Relabel OK as "Cancel"; disable the controls listed in 5.3.3; clear the branch link.
2. Refresh the index stat data (`RefreshGitIndex`).
3. Store the scroll position; clear the list.
4. Set `m_bIncludedStaged`; call `GetStatus` with:
   - the path list (or none in whole-project mode);
   - `bUpdate` = registry `CheckRepo` (default FALSE);
   - the ignored, unversioned and local-changes-ignored flags from the checkboxes.

   On failure, the list's error text becomes the empty-list text.
5. `Show(VERSIONEDBUTNORMALANDEXTERNALS | [UNVERSIONED] | [IGNORED] | [ASSUMEVALID|SKIPWORKTREE])` with no check mask.
6. Update the statistics and the title; set the branch link to the current branch; relabel OK as "OK"; re-enable the controls.

**While loading**, the list paints "Please wait..." [POT:7093].

**Empty result.** When nothing is shown, it paints "No files to show with the current setting.\nCheck one or more settings below to see unversioned, ignored and/or unmodified files." [POT:6423; CHD:104]

**Reload triggers**:

- dialog opening;
- Refresh and F5;
- turning on Show unversioned / ignored / local-changes-ignored when that data is not loaded yet;
- Show all staged files;
- Show Whole Project;
- Commit, Stash and Switch;
- any list command that sends `GITSLNM_NEEDSREFRESH` (Add, Revert of added files, Ignore, Delete with index changes, LFS, Resolve with errors, and so on). [CHD:311-315]

The dialog does not watch the file system.

#### 5.3.6 Statistics line

`IDC_INFOLABEL` shows the full statistics string of 5.2.7 with " = " replaced by "=" and newlines replaced by ", ". [CHD:357-363] The format is:

```
line: <added>(+) <removed>(-) files: normal=0, non-versioned=N, modified=N, added=N, deleted=N, conflicted=N
```

It is updated after each load, after each checkbox re-filter, and on `GITSLNM_ITEMCOUNTCHANGED`. [CHD:317-321] The control's "n files selected, n files total" counter is **not** shown in this dialog, because no stat label is attached.

#### 5.3.7 The file list in this dialog

- **Init.** `Init(COLEXT | COLSTATUS | COLADD | COLDEL | COLMODIFICATIONDATE, L"ChangedDlg", GITSLC_POPALL ^ (POPSAVEAS | POPRESTORE | POPPREPAREDIFF), false)`. This means no checkboxes, `bHasWC` = true, and all columns allowed. [CHD:101]
- **Default columns**: **Path | Extension | Status | Lines added | Lines removed | Last Modified**. Filename and Size can be added from the header menu, and LFS Lock Owner too when the repository has LFS. Settings are stored under `StatusColumns\ChangedDlg`.
- **Rows** use the colours of 5.2.5. Groups appear as soon as unversioned, ignored or flagged files are shown, or changelists exist. The list is multi-select (no `LVS_SINGLESEL`) [RC:193] and unsorted until a header is clicked.
- **Context menu**: everything in 5.2.8 except "Save revision to...", "Mark for comparison"/"Diff with" and "Restore after commit"/"Restore", which the mask removes. Compared with the Log dialog's list (1.11.2), this dialog **adds the changelist items** (Log removes them) and **removes Save revision to... and Mark for comparison**. Because the list has no checkboxes, right-clicking a group header shows no menu.
- **Double-click and Enter**: working tree against HEAD in the diff tool, as in 5.2.9.
- **Drag and drop**: dragging out works; dropping in is refused, because `EnableFileDrop` is never called.
- **Keys**: as in 5.2.9, plus F5 for the dialog.

#### 5.3.8 Subfolder, root, file, several paths

- **Repository root** (relative path empty): "Show Whole Project" is forced checked and **disabled** [CHD:93-97]. This forced value is not written to the registry. The title shows the repository root. Show unversioned and Show ignored are enabled.
- **Subfolder**: the git commands get the folder as pathspec (5.2.2). Staged files elsewhere still appear while "Show all staged files" is on. "Show Whole Project" is enabled; its per-repository saved value may already be on, in which case the whole repository is listed and the title shows the root. Paths stay relative to the repository root in the Path column; they are not made relative to the subfolder.
- **Single file**: the same, with that file as pathspec. Show unversioned and Show ignored are disabled after loading. Paths passed explicitly are remembered as "direct files" (`m_setDirectFiles`). That only matters for dialogs that pass `SHOWDIRECTFILES` in their check mask, which this one does not. [GSLC:406-412]
- **Several paths** (`/pathfile`): each git command runs once per path and the outputs are concatenated [GIT:3478-3524; TGP:1126-1166]. The title uses the common directory, and Show unversioned and Show ignored are disabled after loading. Commit and Save unified diff use the common root's directory.

### 5.4 Commit dialog (`CCommitDlg`, `IDD_COMMITDLG`)

#### 5.4.1 Entry and window

- **Entry.** `/command:commit` takes the paths (`/path` or `/pathfile`), plus `/logmsg:<text>` or `/logmsgfile:<file>` (not both), `/bugid:<id>` and `/wholeproject`. [CC:27-56] `CAppUtils::Commit` first checks that `user.name` and `user.email` are set (`CheckUserData`). It then shows the dialog once and, after it closes with OK, runs the chosen post-commit action (5.4.8). [AU:3062-3140]
- **Template.** `IDD_COMMITDLG DIALOGEX 0,0,359,318`, caption "Commit", resizable, with minimize and maximize boxes. [RC:245-248]
- **Title and placement.** The title is `<path> - Commit - TortoiseGit`, with the same path rules as 5.3.1 [CD:1551-1565]. Size and position are saved (`EnableSaveRestore(L"CommitDlg")`). The splitter position is saved as `HKCU\Software\TortoiseGit\TortoiseProc\ResizableState\CommitDlgSizer`, unscaled. [CD:410-430, 1331-1341]
- **Whole project forced.** `m_bWholeProject2` (whole project, fixed) is set when the path list is empty, or is the single empty path (the repository root). [CD:230-234]

#### 5.4.2 Layout (top to bottom)

Controls from RC:249-297; anchors from CD:360-406.

| Control | ID | Label / type | Rect (DLU) | Notes |
|---|---|---|---|---|
| "Commit to:" | `IDC_COMMITLABEL` | static | 6,7,46,8 | |
| Branch | `IDC_COMMIT_TO` | read-only borderless edit | 48,7,68,12 | current branch, set after each load [CD:1433] |
| New branch name | `IDC_NEWBRANCH` | edit, hidden | 46,5,68,12 | replaces the branch edit when "new branch" is checked [CD:3026-3041] |
| "new branch" | `IDC_CHECK_NEWBRANCH` | auto checkbox | 117,5,66,12 | disabled while a merge is in progress [CD:475-478] |
| "Bug-ID/Issue-Nr:" | `IDC_BUGIDLABEL` | static, right-aligned | 177,6,68,8 | shown only when the bugtraq message (`sMessage`) is configured; the text comes from the bugtraq label when one is set [CD:302-320] |
| Bug ID | `IDC_BUGID` | edit | 249,3,46,14 | same condition; Enter moves focus to the message [CD:1671-1677] |
| "{BugTraq}" | `IDC_BUGTRAQBUTTON` | button, hidden and disabled | 299,3,53,14 | shown with the provider's link text when an issue-tracker provider is associated [CD:277-301] |
| "&Message:" | `IDC_MESSAGEGROUP` | group box | 6,19,346,135 | |
| Message | `IDC_LOGMESSAGE` | Scintilla | 13,32,331,72 | 5.4.3 |
| line/column | `IDC_TEXT_INFO` | static, right-aligned | 299,105,45,8 | `<line>/<column>`, 1-based, updated on every caret move [CD:2836-2848] |
| "Amend &Last Commit" | `IDC_COMMIT_AMEND` | auto checkbox, initially disabled | 14,111,135,10 | tooltip [POT:10189] |
| "Show diff to last commit" | `IDC_COMMIT_AMENDDIFF` | auto checkbox, hidden and disabled | 212,110,132,10 | shown while amending |
| "Set author &date" | `IDC_COMMIT_SETDATETIME` | auto checkbox | 14,125,102,10 | |
| Date / Time | `IDC_COMMIT_DATEPICKER` / `IDC_COMMIT_TIMEPICKER` | date picker / time picker (up-down), hidden | 123,123,66,13 / 193,123,61,13 | shown while "Set author date" is checked; range 1970-01-01 to 2099-12-31 [CD:238-240] |
| "Reset" | `IDC_COMMIT_AS_COMMIT_DATE` | auto checkbox, hidden and disabled | 259,125,85,10 | shown while amending with a date set; tooltip "Use the current time by setting the author date to the commit date." [POT:10163] |
| "Set au&thor" | `IDC_COMMIT_SETAUTHOR` | auto checkbox | 14,139,102,10 | |
| Author | `IDC_COMMIT_AUTHORDATA` | edit (read-only until "Set author" is checked) | 123,137,119,13 | `user.name <user.email>`; HEAD's author while amending [CD:1521-1535, 3081-3097] |
| "Add &Signed-off-by" | `IDC_SIGNOFF` | button | 258,137,86,13 | |
| Splitter | `IDC_SPLITTER` | `CSplitterControl` | 6,157,346,5 | moves the message group against the list group; range from 120 px below the message top to 80 px above the list bottom [CD:2599-2612, 2614-2704] |
| "Changes made (F5: refresh, double-click on file for diff):" | `IDC_LISTGROUP` | group box | 6,162,346,126 | |
| "Check:" + links | `IDC_SELECTLABEL`, `IDC_CHECKALL`… | static + link controls: "&All", "&None", "Unversioned", "Versioned", "Added", "Deleted", "Modified", "Files", "Submodules" | y=172 | laid out left to right, 9 px apart [CD:344-355] |
| File list | `IDC_FILELIST` | `CGitStatusListCtrl` **with checkboxes** | 14,184,330,62 | |
| "Staging support (EXPERIMENTAL)" | `IDC_STAGINGSUPPORT` | auto checkbox | 14,252,180,10 | |
| "Show &Unversioned Files" | `IDC_SHOWUNVERSIONED` | auto checkbox | 14,263,135,10 | |
| Statistics | `IDC_STATISTICS` | static, right-aligned | 149,247,195,8 | "%1!d! files selected, %2!d! files total"; its tooltip is the statistics string [CD:252, 1434] |
| "View Patch>>" | `IDC_VIEW_PATCH` | link | 259,260,76,8 | becomes "Hide Patch<<" [POT:10310, 5049] |
| "Partial Staging>>" / "Partial Unstaging>>" | `IDC_PARTIAL_STAGING` / `IDC_PARTIAL_UNSTAGING` | links, hidden | 259,268 / 259,276 | shown in staging mode instead of View Patch; they become "Hide Staging<<" / "Hide Unstaging<<" [POT:6923, 6927, 5053, 5057] |
| "Do not autoselect submodules" | `IDC_NOAUTOSELECTSUBMODULES` | auto checkbox | 14,274,147,10 | |
| "Show &Whole Project" | `IDC_WHOLE_PROJECT` | auto checkbox | 14,290,109,10 | disabled when `m_bWholeProject2` [CD:1480] |
| "Message onl&y" | `IDC_COMMIT_MESSAGEONLY` | auto checkbox | 14,301,107,10 | tooltip [POT:1709] |
| Merge active | `IDC_MERGEACTIVE` | icon, hidden | 147,291,18,17 | shown while `MERGE_HEAD` exists; tooltip [RC:4052] |
| "C&ommit" | `IDOK` | `CMenuButton`, initially disabled | 171,297,69,14 | 5.4.7 |
| "Cancel" | `IDCANCEL` | button | 247,297,50,14 | |
| "Help" | `IDHELP` | button | 302,297,50,14 | |

There are no collapsible sections; the only layout control is the splitter. When a merge is in progress, an information box says the commit is a merge commit [POT:9260]; it can be suppressed with "do not show again" (`CommitMergeHint`). [CD:475-481]

#### 5.4.3 Message editor

- **Initial text.** When no message was passed in, the editor starts with the file named by `commit.template` (unless amend is forced). If `<worktree admin dir>SQUASH_MSG` and then `MERGE_MSG` exist, they are loaded in that order. [CD:156-205] The caret is placed at the start. [CD:322-327]
- **Font.** The log font (`CAppUtils::GetLogFontName/Size`). [CD:261]
- **Width marker.** With `tgit.logwidthmarker` = N, the editor draws an edge line at column N and turns word wrap **off**. Without it, it wraps by word. [SCE:339-361; PP:92-97]
- **Spell checking** is on unless `Spellchecker` is 0. It tries the Windows spell checker for the user or project language first, and hunspell dictionaries otherwise. Suggestions appear as an autocompletion list. [SCE:247-317, 744-783]
- **Autocompletion** is on unless `Autocompletion` is 0. It opens after `AutoCompleteMinChars` characters (default 3). [CD:1469-1474; SCE:245] The word list is built after each load on the status thread and holds [CD:1939-2136]:
  - every listed path, and each of its suffixes after a `/` (plus the name without extension when `AutocompleteRemovesExtensions` is set);
  - identifiers matched in the listed files' contents by the per-extension regexes in `autolist.txt` (application directory, then the user's app-data directory). Unversioned files are skipped unless `AutocompleteParseUnversioned` is set. Files above `AutocompleteParseMaxSize` (300000 bytes) are skipped, and parsing stops after `AutocompleteParseTimeout` seconds (5);
  - snippets from `snippet.txt`, which insert their expansion (`\t`, `\n`, `\r`, `\\` are escapes).
- **Styling.** `StyleCommitMessages` (default on) styles `*bold*`-style markup, as in 1.13. Bug IDs and URLs are recognised through the bugtraq regexes. [SCE:244, 339-345]
- **Context menu.** The editor's own context menu gets these items added [CD:2139-2165, 2167-2261]:
  - "Pick commit &hash" [POT:7024]: a Log dialog in single-select mode; inserts the chosen hash
  - "Pick commit &message" [POT:7028]: inserts the chosen commit's subject + CRLF + body
  - "Paste &filename list" [POT:6943]: one line per checked row, `%-10s %s` (status, path), with unversioned rows shown as "Added"
  - only when the history is not empty: "Paste &last commit message" [POT:6947] and "Paste r&ecent message..." [POT:6951] (the history dialog)
- **Message history.** At most `MaxHistoryItems` (default 25) entries [CD:223], stored under `HKCU\Software\TortoiseGit\History\commit<m_ListCtrl.m_sUUID>` (':' replaced by '_'), value name `logmsgs` [CD:1348-1354]. A message is added after a commit, and on cancel when it was changed. While amending, the replaced pre-amend text is added too. [CD:1069-1080, 1601-1612]
- **Keys** [RC:3486-3490; CD:1641-1684]:
  - Alt+M focuses the message and Alt+F the file list (`IDR_ACC_COMMITDLG`).
  - **Ctrl+Enter** commits, if Commit is enabled.
  - F5 refreshes, unless a commit is running.

#### 5.4.4 Options: labels, defaults, persistence, git mapping

| Option | Default | Persisted | Effect |
|---|---|---|---|
| Amend Last Commit | off; enabled after the first load unless a cherry-pick is in progress; forced off in a repository without commits; forced on by `m_bForceCommitAmend` [CD:1484-1499] | no | Puts HEAD's subject + "\n" + body in the editor; the previous text is kept and restored when unticked. Shows "Show diff to last commit". Reloads the list against `HEAD~1` (`m_ListCtrl.m_amend`). The author field shows HEAD's author. Passes `--amend`. [CD:2747-2789, 859-860, 1403] |
| Show diff to last commit | off | no | When on, the list and index are computed against HEAD instead of `HEAD~1`, so only the new changes show. It lets Commit be enabled with nothing checked. After the reload, it is enabled when HEAD has a parent; when HEAD has no parent it is forced on. [CD:1500-1518, 2537, 2974-2978] |
| Set author date | off | no | Shows the date and time pickers, initialised to now, or to HEAD's author date while amending. Passes `--date=<YYYY-MM-DD>T<HH:MM:SS>` (local time, no zone). [CD:2986-3018, 862-879] |
| Reset (only while amending with a date) | off | no | Disables the pickers and passes `--date=now`. [CD:875-876, 3020-3024] |
| Set author | off | no | Makes the author field editable. Passes `--author=<quoted text>`. [CD:880-892] |
| Add Signed-off-by (button) | – | – | Appends `Signed-off-by: <user.name> <<user.email>>` if not already present. It trims trailing whitespace, adds an empty line unless the last line already contains "-by: ", and ends with CRLF. [CD:2715-2745] |
| new branch | off | no | Before committing: `git.exe branch -- <name>` then `git.exe checkout --end-of-options <name> --`. The name is validated, existing branches are refused, and a tag with the same name asks first. [CD:602-626, 777-797] |
| Message only | off | no | Disables the list. Commits with `--allow-empty`, and no files are staged or reset. [CD:2791-2796, 893] |
| Show Unversioned Files | `AddBeforeCommit` (TRUE) | yes: on click, and again on OK [CD:1065] | Lists unversioned files, unchecked by default. [CD:1415-1416, 1727-1750] |
| Do not autoselect submodules | `DoNotAutoselectSubmodules` (FALSE) | yes, **on OK only** [CD:1066] | Submodule (directory) rows are not checked automatically; reloads. [CD:225-226, 1405, 2980-2984] |
| Show Whole Project | `ShowWholeProject\<repo>` (FALSE), shared with 5.3 | yes, on click | Loads without path restriction. [CD:2798-2824] |
| Staging support (EXPERIMENTAL) | git config `tgit.commitstagingsupport` | yes: written to the repository's local git config on toggle [CD:3102; `Git.h:419` (`SetConfigValue` defaults to `CONFIG_LOCAL`)] | Turns on three-state checkboxes that stage and unstage at once (5.2.6), shows the Partial Staging and Partial Unstaging links instead of View Patch, and skips the index rewrite at commit time. Shows a hint once [POT:10415]. [CD:3099-3138] |
| View Patch | git config `tgit.commitshowpatch` | yes | Opens a docked patch window showing `git.exe diff HEAD[~1] -- <file>` for the selected rows. In staging mode it shows "index to working tree" or "HEAD to index" (with `--cached`) for one file, and allows hunk staging. [CD:2409-2488, 2862-2935] |
| Keep changelists (list context menu) | `KeepChangeLists` (FALSE) | yes | When off, files that were committed are removed from their changelists after a successful commit. [CD:995-999] |

**"Check:" links.** Each link calls `Check(mask, true)`; Shift+click unchecks instead. [CD:2504-2528] The masks are:

| Link | Mask |
|---|---|
| All | `SHOWEVERYTHING` |
| None | `SHOWEVERYTHING` (unchecks) |
| Unversioned | `SHOWUNVERSIONED` |
| Versioned | `SHOWVERSIONED` |
| Added | `SHOWADDED` |
| Deleted | `SHOWREMOVED` |
| Modified | `SHOWMODIFIED` |
| Files | `SHOWFILES` |
| Submodules | `SHOWSUBMODULES` |

Each link is enabled only when the list has entries of its kind. [CD:3068-3079]

#### 5.4.5 The file list in this dialog

- **Init.** `Init(COLEXT | COLSTATUS | COLADD | COLDEL, L"CommitDlg", GITSLC_POPALL ^ (POPCOMMIT | POPSAVEAS | POPPREPAREDIFF), true, true)`. It has checkboxes, the stat label `IDC_STATISTICS`, the empty text "No files were changed or added since\nthe last commit." [POT:6427], file drops enabled, and the background `IDI_COMMIT_BKG`. [CD:251-256]
- **Context menu.** It has no "Commit...", "Save revision to..." or "Mark for comparison". It keeps "Restore after commit"/"Restore": marked files are copied back after the commit or on cancel, after asking [POT:10585] [CD:3043-3066]. It keeps the changelist items.
- **Load** [CD:1356-1549]: `m_bIncludedStaged` = true, so all staged files of the repository are listed. `m_amend` follows the amend options. `GetStatus(paths or none, ..., bGetStagingStatus = staging mode)` is called, plus the unversioned list when shown. The flags are then:
  - `dwShow` = `VERSIONEDBUTNORMALANDEXTERNALSFROMDIFFERENTREPOS | INCHANGELIST | DIRECTFILES`, plus `UNVERSIONED` when `AddBeforeCommit` is set.
  - `dwCheck` = `dwShow` without UNVER when `SelectFilesForCommit` is on, else 0.

  So by default every versioned change is checked and every unversioned file is unchecked. Files in `ignore-on-commit` are unchecked, and submodules too when "Do not autoselect submodules" is on.
- **Nothing to commit.** When nothing is listed but unversioned files exist (and no merge is in progress), a box asks "No files were changed or added since\nthe last commit.\nDo you want to see the unversioned files?" [POT:6431]. Yes ticks Show Unversioned Files. [CD:1446-1458]
- **Dropping files** from Explorer that belong to the same repository runs `git.exe add -- <path>` and adds the path to the path list if it is not covered yet. The list refreshes 200 ms later, debounced. [CD:1780-1843, 2281-2293]
- **Meaning of a checked row (non-staging mode).** Checked rows are what gets committed. Before `git commit`, the index is rewritten to match the check state (5.4.6).

#### 5.4.6 Commit sequence (OK)

`OnOK` [CD:569-1098]:

1. If the status thread is running, cancel it.
2. If any row is conflicted: "One or more files are in a conflicted state." [POT:6690], with OK and Ignore buttons (`CommitWarnOnUnresolved`). OK selects the first conflicted row and stops.
3. Validate the new branch name, and validate the bug ID ("Only numbers (optionally separated by commas)\nare allowed!" [POT:6722]).
4. Check the message:
   - An empty message is refused: "You must enter a log message for the commit" [POT:10589].
   - With `bugtraq.warnifnoissue`: "You haven't entered an issue number! ..." [POT:10577].
   - When the template was not edited: [POT:10551], with "do not show again" remembering only "proceed".
   - With `tgit.warnnosignedoffby` and no sign-off line: "You haven't entered your Signed-Off-By line!" [POT:10581], with buttons "&Add it", "&Commit w/o" and Abort.
   - Conflict-marker hints in the message stop the commit (`MessageContainsConflictHints`).
5. **pre-commit hook** (TortoiseGit client hook), with the checked paths and the message. On a non-zero exit code, a task dialog "Hook script failed" offers "&Abort" (the default) or "&Proceed anyway". [POT:9350, 5105, 295, 982]
6. **Dirty checked submodules.** For each checked submodule, `git.exe diff -- <sub>` ending in `-dirty`, or `git.exe status --porcelain` inside an unversioned one, asks [POT:9460] with Commit / Ignore / Cancel. Commit starts a separate commit dialog for the submodule and stops this one.
7. **Index preparation** (skipped in staging mode and with Message only). A "Preparing commit..." / "Updating index" progress window appears for 25 or more rows. [CD:1100-1329]
   - **libgit2 path** (`UsingLibGit2(GIT_CMD_COMMIT_UPDATE_INDEX)`): each checked row is added to the index by path (deleted rows are removed), and a renamed row's old path is removed. Each unchecked versioned row is reset to its entry in HEAD's tree (`HEAD~1` when amending without "Show diff to last commit"). Unchecked added and renamed rows are removed from the index and queued for re-adding.
   - **git.exe path**, for checked rows:
     - unversioned files, and directories that are not deleted: `git add -f`
     - deleted files: `git update-index --force-remove`
     - other checked files: `git update-index`
     - a renamed row's old path: `git rm --ignore-unmatch`
   - **git.exe path**, for unchecked rows:
     - added or renamed: `git rm -f --cache`, plus a re-add queued; for a rename, its old path goes through `git reset [HEAD~1]` and a re-delete is queued
     - other versioned rows: `git reset [HEAD~1]`
   - These run as `CMassiveGitTask` batches; how the batches are split could not be established from source (5.6).
8. Create and check out the new branch, if requested.
9. **Detached HEAD.** "Current HEAD is detached, you are working on (no branch).\nDo you want to create a branch now?" [POT:3278] with Yes/No/Cancel. Yes opens the create-branch dialog.
10. **Bug ID into the message.** The bug ID is written into the message with the bugtraq template (`%BUGID%`), appended or prepended. Then the provider's `CheckCommit` runs. [CD:1617-1639, 802-839]
11. **Commit or stop.** It commits when Message only is on, or when preparation succeeded and something is checked (or staging mode, amend, or a merge in progress). Otherwise it shows "Nothing to commit" [POT:6561] titled "Commit Finish" [POT:2722]. [CD:841, 1058-1062]
12. **The commit.** The message is written as UTF-8 to a temp file. Then the dialog runs **`git.exe commit [--author=<a>] [--date=<d>] [--amend] [--allow-empty] -F <tempfile>`** in the git progress dialog (`CProgressDlg`, command line not shown). That dialog auto-closes when there were no errors and the chosen button entry is not plain Commit. [CD:845-919] Its post-command buttons are:
    - "SVN DCommit..." (git-svn repositories only)
    - "Pus&h..."
    - "&Pull..."
    - "&ReCommit"
    - "Create &Tag..."

    [POT:8010, 7277, 994, 1006, 3170]

    They are offered only on success and when post actions are allowed.
13. **After the command**:
    1. Issue-tracker `OnCommitFinished` runs.
    2. "Restore after commit" files are copied back.
    3. The queued re-add and re-delete batches run (`git add --ignore-errors -f`, `git rm --cached --ignore-unmatch`), unless `ReaddUnselectedAddedFilesAfterCommit` is 0. They restore the user's staging of unchecked added files.
    4. The **post-commit hook** runs; a failure keeps the dialog open.
    5. Changelists are pruned unless Keep changelists is on.

    [CD:928-1000]
14. **Close or stay.** On failure or ReCommit the dialog stays open. For ReCommit, the message resets to the template, the amend, date and author options reset, the start-commit hook runs again and the list reloads. Otherwise the dialog closes; with "Commit && &Push" selected, the push happens next (5.4.7). [CD:1002-1098]

The **start-commit hook** runs when the dialog opens, before anything is loaded, and again on ReCommit. If it fails and the user aborts, the dialog closes. [CD:209-213, 3140-3162]

**Status thread** (`StatusThread`, started on open, F5, and when options change) [CD:1356-1549, 1686-1725]:

- It disables Commit, the options and the links while it runs.
- It refreshes the index and loads the list (above), then sets the branch, title, author and links.
- It builds the autocompletion list.
- Cancelling or committing while it runs waits up to 5 s for it to stop, then kills it.

#### 5.4.7 Buttons

- **Commit button.** "C&ommit" (`IDOK`) is a `CMenuButton` with three entries: "C&ommit", "&ReCommit" and "Commit && &Push" [POT:2234, 1006, 2710]. The last-used entry is the default, persisted as `CommitLastAction` (default 0). Each entry's mnemonic with **Alt** (Alt+O, Alt+R, Alt+P) selects that entry and commits at once. [CD:491-530, 2576-2593, 921-922] When post actions are disabled, the entries are not added.
  - **Commit** commits and closes. The progress dialog's buttons can then pick DCommit, Push, Pull or Create Tag, which run after the dialog closes. ReCommit from the progress dialog keeps it open. [AU:3100-3116]
  - **ReCommit** commits, keeps the dialog open and reloads. [CD:923-924, 1090-1095]
  - **Commit & Push** commits, closes, then pushes. If the current branch has a push remote and branch configured and this was not an amend, it pushes directly (`CAppUtils::DoPush`). Otherwise it opens the Push dialog. [CD:553-567, 1086-1088]
- **Enabled state.** Commit is enabled only when the trimmed message is not empty and at least `tgit.logminsize` characters long, **and** one of these holds [CD:2530-2542]:
  - Message only is on;
  - at least one row is checked;
  - amend with "Show diff to last commit" is on;
  - a merge is in progress.
- **Cancel** asks "Are you sure you want to cancel?\n\nHint: Recently used commit messages are available using the context menu of the text field." [POT:1834, 5089] when the message was changed or the list is not empty. No is the default button, and there is a "do not show again" (`CommitAskBeforeCancel`). Cancel then offers to restore "Restore after commit" files and saves a changed message to the history. [CD:1567-1615]
- **Help** opens the help page; its handler was not examined.

#### 5.4.8 What runs after the dialog closes

`CAppUtils::Commit` runs `m_PostCmd`, which was chosen in the progress dialog [AU:3100-3116]:

| `m_PostCmd` | Runs |
|---|---|
| DCommit | `SVNDCommit` |
| Push | `CAppUtils::Push` (the Push dialog) |
| CreateTag | `CreateBranchTag(tag)` |
| Pull | `CAppUtils::Pull` |

The loop around the dialog never repeats: `bFailed` is never set to true. [AU:3071-3138]

### 5.5 Other dialogs that embed the list

| Dialog | Columns visible (allowed) | Context-menu mask | Checkboxes | Show / check masks | Extras |
|---|---|---|---|---|---|
| Log (`IDD_LOGMESSAGE`, 1.11.2) | Ext, Status, +, − (only these) | `POPALL ^ (RESTORE \| CHANGELISTS)` | no | `SHOWVERSIONED` [+ `UNVERSIONED`] on a commit's file list (`UpdateWithGitPathList`) | `bHasWC` = repository not bare [`LogDlg.cpp:308, 1024-1032`] |
| Check for modifications (5.3) | Ext, Status, +, −, Last Modified (all) | `POPALL ^ (SAVEAS \| RESTORE \| PREPAREDIFF)` | no | see 5.3.5 | |
| Commit (5.4) | Ext, Status, +, − (all) | `POPALL ^ (COMMIT \| SAVEAS \| PREPAREDIFF)` | yes; three-state in staging mode | see 5.4.5 | stat label, file drop |
| Revert (`IDD_REVERT`) | Ext, Status, +, − (all) | `POPALL ^ (COMMIT \| RESTORE \| CHANGELISTS)` | yes | show `VERSIONEDBUTNORMALANDEXTERNALSFROMDIFFERENTREPOS \| DIRECTFILES`, check `DIRECTFILES \| ADDED` (only the paths the user passed, and added files) | confirm button = OK, select-all checkbox, file drop [`RevertDlg.cpp:65-70, 117-123`] |
| Add (`IDD_ADD`) | Ext (Ext, Last Modified, Size) | `POPALL ^ (ADD \| COMMIT \| CHANGELISTS \| PREPAREDIFF)` | yes | show `UNVERSIONED \| DIRECTFILES` [+ `IGNORED` when "include ignored" is on], check `UNVERSIONED` | loads unversioned (and ignored); confirm = OK; empty text `IDS_ERR_NOTHINGTOADD` [`AddDlg.cpp:63-70, 145-153`] |
| Resolve (`IDD_RESOLVE`) | Ext (all) | `POPALL ^ (IGNORE \| ADD \| COMMIT \| EXPORT \| RESTORE \| SAVEAS \| PREPAREDIFF \| CHANGELISTS)` | yes | show `CONFLICTED`, check `CONFLICTED` | confirm = OK, file drop [`ResolveDlg.cpp:60-65, 143-145`] |
| LFS locks (`IDD_LFS_LOCKS`) | Ext, LFS Lock Owner (Ext, Filename, Size, Last Modified, LFS Lock Owner) | `POPLFSLOCK \| POPLFSUNLOCK` only | yes | show and check `SHOWLFSLOCKS` | loads with `bShowLFSLocks`; confirm = Unlock button [`LFSLocksDlg.cpp:61-63, 107-109`] |
| Rebase / Cherry-pick (`IDD_REBASE`) | Ext, Status, +, − (only these) | `POPALL ^ (COMMIT \| RESTORE \| CHANGELISTS)` | no | conflict view: `GetStatus(nullptr, true)`, show UNMERGED\|MODIFIED\|ADDED\|DELETED\|REPLACED, check UNMERGED; commit view: `SHOWVERSIONED` on a commit's files | [`RebaseDlg.cpp:243, 2468-2469, 2937-2944`] |
| Sync, in/out change lists (`IDD_SYNC`) | Ext, Status, +, − (only these) | only `COMPARETWOREVISIONS \| GNUDIFF2REVISIONS` | no | two-revision diff lists (`m_Rev1`/`m_Rev2`); double-click runs `StartDiffTwo` | `bHasWC` = false [`SyncDlg.cpp:1053-1055, 1102-1104`] |
| Sync, conflict list | Ext, Status, +, − (all) | `EXPLORE \| OPEN \| SHOWLOG \| CONFLICT \| RESOLVE` | no | show and check UNMERGED, after `GetStatus(nullptr, true)` | [`SyncDlg.cpp:1068-1070, 286-289`] |

These dialogs were searched for an embedded `CGitStatusListCtrl` and none was found: Stash save, Delete and the Diff (`FileDiffDlg`) window. The only embedding dialogs found are the ones above. [grep of `src/TortoiseProc` for `CGitStatusListCtrl` members]

### 5.6 Not established from source

- **Double-click on an unversioned or ignored file.** It calls `CGitDiff::Diff(file, file, WorkingCopyRef, m_CurrentVersion.ToString())`, and in WC mode the second revision is the empty hash's string. `CGitDiff::Diff` then resolves it with `"^{}"` appended [GD:376]. Whether that succeeds, and what the user then sees (an error box or a diff against an empty file), was not established.
- **Added files in WC mode.** `StartDiff` passes `m_CurrentVersion.ToString()` (empty) to `DiffNull` for an added file. How `DiffNull` treats that revision was not examined.
- **The unified-diff command.** The exact git command behind "Save unified diff" and "Show changes as unified diff" is built by `CGit::GetUnifiedDiffCmd`, which was not examined. The unified diff viewer's own UI (where saving happens) was not examined either.
- **What the list shows after reverting an added file.** Reverting an added file triggers a full refresh. What appears after that depends on the parent dialog.
- **Group ordering and empty groups.** Whether Windows list views hide the always-empty "(no changelist)" group, and how groups are ordered on screen, depends on `ListView` behaviour that was not examined.
- **Mnemonics.** Whether the hidden "Show un&modified files" checkbox still takes Alt+M.
- **Dialog persistence.** Where `EnableSaveRestore` stores the dialog rectangle. The commit splitter key suggests `HKCU\Software\TortoiseGit\TortoiseProc\ResizableState\`, but the generic code was not examined.
- **`CMassiveGitTask`.** How it splits long file lists into several git calls, and whether it inserts `--`.
- **Folder Diff on a submodule.** Which repository becomes current when a folder Diff is run on a submodule's root folder (no `/submodule` is passed from Explorer). `SetCurrentDir` was not examined.
- **Help.** The Help button's handler and the help topic it opens.
- **History key.** The value of `m_sUUID`, used in the commit-message history key.

## 6. Add, Revert and Resolve dialogs

The three dialogs have the same shape: a `CGitStatusListCtrl` with checkboxes (5.2), a tri-state "Select/deselect all" checkbox, and OK, Cancel and Help. OK does no git work itself. It only stores the checked entries, and the command class then runs a progress command in the Git progress dialog (`CGitProgressDlg`, 6.4). The list's own configuration (masks and columns) is summarised in 5.5. This section adds what 5.5 leaves out.

New citation keys (same commit as section 0):

| Key | File |
|---|---|
| **ADC** | `src/TortoiseProc/Commands/AddCommand.cpp` |
| **RVC** | `src/TortoiseProc/Commands/RevertCommand.cpp` |
| **RSLC** | `src/TortoiseProc/Commands/ResolveCommand.cpp` (`RSC` is taken by RepoStatusCommand) |
| **ADD** / **ADD.h** | `src/TortoiseProc/AddDlg.cpp` / `.h` |
| **RVD** / **RVD.h** | `src/TortoiseProc/RevertDlg.cpp` / `.h` |
| **RSD** / **RSD.h** | `src/TortoiseProc/ResolveDlg.cpp` / `.h` (`RD` is taken by RebaseDlg) |
| **APC** / **APC.h** | `src/TortoiseProc/ProgressCommands/AddProgressCommand.cpp` / `.h` |
| **RVPC** / **RVPC.h** | `src/TortoiseProc/ProgressCommands/RevertProgressCommand.cpp` / `.h` |
| **RSPC** / **RSPC.h** | `src/TortoiseProc/ProgressCommands/ResolveProgressCommand.cpp` / `.h` |
| **MGT.h** | `src/TortoiseProc/MassiveGitTask.h` |
| **MGTB** | `src/Git/MassiveGitTaskBase.cpp` (batches one git command over many paths) |
| **GPD** | `src/TortoiseProc/GitProgressDlg.cpp` |
| **GPL** | `src/TortoiseProc/GitProgressList.cpp` |
| **SU** | `src/Utils/ShellUpdater.cpp` (overlay refresh) |
| **GIT.h** | `src/Git/Git.h` |
| **TMF** | `src/TortoiseMerge/MainFrm.cpp` |

### 6.1 Shared behaviour

- **Entry points.** Each dialog is opened by a TortoiseGitProc command: `add`, `revert` and `resolve`. Explorer sends `/pathfile:<tmp>` for all three (4.2 rows 48, 39 and 34). None of the three commands overrides the default path requirement, *working tree required*. Outside a working tree, TortoiseGitProc shows "No working directory found." [POT:6485] and stops. [CMDH:96; CMDC:454-462] The paths arrive relative to the repository root, and the root itself becomes the empty path (5.1).
- **Window.** All three are `CResizableStandAloneDialog`s with `WS_THICKFRAME` and minimize/maximize boxes. [RC:175, 569, 801] They are centred on the Explorer window when one is known, and save and restore their placement under the names "AddDlg", "RevertDlg" and "ResolveDlg". [ADD:84-86; RVD:82-84; RSD:77-79] The list stretches both ways. "Select/deselect all" is anchored bottom-left and widened to fit its text. Everything else is anchored bottom-right. [ADD:74-82; RVD:74-81; RSD:69-76] The title is `<path> - <caption> - TortoiseGit`, formatted as in 5.3.1 (`DialogTitles`, compaction). [AU:3518-3565]
- **Loading.** The dialog loads on a worker thread: OK is disabled, `StoreScrollPos`, `Clear`, `GetStatus`, then `Show`. If `GetStatus` fails, the list's empty text becomes the git error (`GetLastErrorMessage`). If the thread cannot start, the dialog shows "Could not start thread!" [POT:3141]. [ADD:89-96, 140-157; RVD:87-94, 104-141; RSD:82-89, 133-149]
- **OK, Cancel, Esc.** OK and Cancel do nothing while loading is in progress. Cancel during loading only sets the control's cancel flag (`SetCancelBool`). [ADD:100-118; RVD:143-188; RSD:94-112] The control enables OK only when at least one row is checked (`SetConfirmButton`, 5.2.7).
- **Keys.** Ctrl+Enter presses OK when it is enabled. F5 reloads. [ADD:159-184; RVD:205-237; RSD:151-183]
- **"Select/deselect all"** (`BS_AUTO3STATE`). The control keeps its state in step: unchecked, indeterminate or checked for none, some or all rows checked (5.2.7). When the user clicks it, a box that would go to indeterminate is forced to unchecked. Then `SelectAll(checked)` checks or unchecks **every visible row**. [ADD:120-133; RVD:190-203; RSD:114-127; GSLC:3256-3284]
- **File drop.** Files and folders dropped on the list are accepted only when they belong to the same repository (`HasAdminDir` equals `g_Git.m_CurrentDir`). A dropped path is added to the dialog's path list unless it is already a row, or (when the list holds folders) already lies under one of them. Its stored check state is reset, and a 200 ms timer reloads the list, re-arming itself while a load is running. [ADD:198-278; RVD:251-320; RSD:197-266]
- **Refresh requests** from the list's context menu (`GITSLNM_NEEDSREFRESH`, 5.2.9) start a reload unless one is already running. [ADD:186-196; RVD:239-249; RSD:185-195]
- **Background images.** `IDI_ADD_BKG`, `IDI_REVERT_BKG` and `IDI_RESOLVE_BKG`. The progress dialog reuses the same image for each command. [ADD:69; RVD:69; RSD:64; APC:32; RVPC:37; RSPC:33]
- **What OK hands on.** `WriteCheckedNamesToPathList` replaces the dialog's path list with the checked entries, sorted by path. The entries are full `CTGitPath` objects, **status bits (`m_Action`) and old path included**, and the progress commands rely on that. [GSLC:3579-3591]
- **Help.** The Help button exists in all three templates. Its handler and help topic were not examined.

### 6.2 Add (`CAddDlg`, `IDD_ADD`)

#### 6.2.1 Command (`AddCommand::Execute`)

- **`/noui`.** The branch is compiled out (`#if 0`), so `/noui` does nothing and returns false. [ADC:30-39]
- **All paths are files** (`AreAllPathsFiles`, i.e. none is an existing directory). **There is no dialog.** The progress dialog runs `AddProgressCommand` on the paths as given. [ADC:42-53; TGP:1445-1449]
- **Otherwise** (at least one folder), `CAddDlg` opens. After OK with a non-empty selection, `AddProgressCommand` runs on the checked entries. [ADC:56-72]
- **Overlays.** The command always flushes the shell updater at the end (6.4). [ADC:75]

#### 6.2.2 Window

`IDD_ADD DIALOGEX 0,0,244,154`, caption **"Add"**, resizable. Controls from RC:174-185:

| Control | ID | Label | Rect (DLU) |
|---|---|---|---|
| File list | `IDC_ADDLIST` | – (report, show-selection-always, border) | 7,7,230,107 |
| Select all | `IDC_SELECTALL` | "Select/deselect &all" (3-state) | 7,119,78,10 |
| Include ignored | `IDC_INCLUDE_IGNORED` | "Include &ignored files" (auto checkbox) | 89,119,78,10 |
| OK | `IDOK` | "OK" (default) | 73,133,50,14 |
| Cancel | `IDCANCEL` | "Cancel" | 131,133,50,14 |
| Help | `IDHELP` | "Help" | 187,133,50,14 |

- **Title.** The path is the common root of the given paths, made absolute (`CombinePath(GetCommonRoot().GetUIPathString())`). [ADD:72]
- **Resizing.** "Include ignored files" is also widened to fit (`AdjustControlSize`). [ADD:75]
- **Taskbar.** The window is marked unpinnable (`MarkWindowAsUnpinnable`). Revert and Resolve do the same. [ADD:60; RVD:63; RSD:58]

#### 6.2.3 The list

- **Init.** Only Extension is visible by default; Extension, Last Modified and Size are available. Container name "AddDlg", with checkboxes. The context menu is `POPALL ^ (ADD | COMMIT | CHANGELISTS | PREPAREDIFF)`. [ADD:63]
- **Placeholder.** The empty text is "There is nothing to add.\nAll the files and folders are either under version control,\nhave been ignored or the global ignore configuration setting." [POT:9521]. [ADD:67]
- **Dead setting.** `SetIgnoreRemoveOnly()` is called [ADD:64], but `m_bIgnoreRemoveOnly` is never read anywhere, so it has no effect. [GSLC.h:452, 839]
- **Load.** `GetStatus(&paths, false, bShowIgnores = includeIgnored, bShowUnRev = true)`, so it loads `FILELIST_MODIFY` + `FILELIST_UNVER` [+ `FILELIST_IGNORE`] (5.2.2). [ADD:148]
- **Show.** `Show(UNVERSIONED | DIRECTFILES | REMOVEDANDPRESENT [| IGNORED], check = UNVERSIONED)`. `SHOWREMOVEDANDPRESENT` is 0 (5.2.3), so the rows are the **unversioned files, plus the ignored files when the box is on**. Modified or staged files are loaded but never shown. [ADD:150-153]
- **Initial checks.** Every unversioned row starts checked. Ignored rows start unchecked, unless the file was passed explicitly on the command line (`DIRECTFILES` pre-check, which applies to non-directory command-line paths [GSLC:406-412, 688-695]). A check state remembered from an earlier load wins (5.2.6).
- **"Include ignored files".** Default unchecked. It is not persisted (plain member, `FALSE`). A click re-reads the box and reloads the list, which adds or drops the ignored files as their own group, "Ignored Files" (5.2.5). [ADD.h:70; ADD:280-284]
- **Directories.** `ls-files --others` lists files, so an unversioned folder appears as its files (5.2.2).

#### 6.2.4 What OK does (`AddProgressCommand`)

The progress window title is `<common root> - Add - TortoiseGit` ("Add" [POT:1563]). The first line of the list is "Command" [POT:2680] / "Add". [APC:31-38; GPL:388-391]

- **With libgit2** (the default: `UseLibgit2` TRUE, and `UseLibgit2_mask` defaults include `GIT_CMD_ADD`) [GIT:240-242; GIT.h:34]:
  1. Open the repository index and `git_index_read(force)`.
  2. For each path, `git_index_add_bypath(path with any trailing '/' trimmed)`, then report a row "Added" [POT:1620] in the Added colour.
  3. `git_index_write` once at the end.

  The first error stops the run (`ReportGitError`); paths already added are then not written to the index. Cancel is checked after each file. [APC:42-100; GPL:1090-1137]
- **Without libgit2.** `git.exe add -f --pathspec-from-file=<tmp> --pathspec-file-nul` with all paths at once. The temp file holds UTF-8, NUL-separated repository paths. [APC:103-105; MGTB:80-128; TGP:1552-1569] `-f` (and libgit2's add, which does not consult ignore rules) is what lets ignored files be added.
- **After success** [APC:118-141; GPD:255-303]:
  - The paths are queued for an overlay update.
  - The progress dialog's menu button offers:
    - "&Commit..." [POT:431], which runs `/command:commit /path:"<repository root>"`. It is on by default (`m_bShowCommitButtonAfterAdd` = true; `AddCommand` does not change it) [APC.h:24].
    - "Add as Executable (+x)" [POT:1588] and "Add as Symlink" [POT:1592]. These rewrite the index entry mode of every non-directory path to `100755` / `120000` through libgit2, whatever the libgit2 setting. Errors show a message box such as "Could not update file mode.". [APC:146-193]
- **Errors.** There is no post-command menu after an error. The info line says "The operation failed." [POT:9403]. [GPL:488-493]

### 6.3 Revert (`CRevertDlg`, `IDD_REVERT`)

#### 6.3.1 Command (`RevertCommand::Execute`)

The dialog always opens, for files and folders alike. On OK, `RevertProgressCommand` (target revision `"HEAD"`, the default) runs on `m_selectedPathList`: the checked rows, sorted by path. **The command returns true whether or not errors occurred.** [RVC:26-41; RVPC.h:24; RVD:143-179]

`m_bRecursive` is computed in OnOK (true only when every row is checked), and in the non-recursive case OnOK also rewrites `m_pathList`. Neither result is used by `RevertCommand`. [RVD:149-175]

#### 6.3.2 Window

`IDD_REVERT DIALOGEX 0,0,279,232`, caption **"Revert"**, resizable. Controls from RC:568-579:

| Control | ID | Label | Rect (DLU) |
|---|---|---|---|
| File list | `IDC_REVERTLIST` | – | 7,7,265,180 |
| Select all | `IDC_SELECTALL` | "Select/deselect &all" (3-state) | 7,189,141,17 |
| Note | `IDC_UNVERSIONEDITEMS` | "Note: the folder contains unversioned items" (static, hidden) | 160,190,112,16 |
| OK | `IDOK` | "OK" (default) | 101,210,50,14 |
| Cancel | `IDCANCEL` | "Cancel" | 161,210,50,14 |
| Help | `IDHELP` | "Help" | 221,210,50,14 |

- **Title.** The path is the absolute common root of the given paths. [RVD:72]
- **The "Note" label.** It would be shown only when `HasUnversionedItems()` is true and `HKCU\Software\TortoiseGit\UnversionedAsModified` is set (default FALSE). [RVD:125-135] However, `m_bHasUnversionedItems` is only ever assigned FALSE [GSLC:461; GSLC.h:493, 791], so **the label is never shown**.
- **Options.** There are no other options in the dialog.

#### 6.3.3 The list

- **Init.** Extension, Status, Lines added and Lines removed are visible; all columns are available. Container name "RevertDlg". The context menu is `POPALL ^ (COMMIT | RESTORE | CHANGELISTS)`, so the list's own "Re&vert..." entry (5.2.8) is still offered. [RVD:65]
- **Placeholder.** No empty string is set, so an empty list shows "File list is empty" [POT:4501].
- **Before loading,** the thread refreshes the index stat data with `RefreshGitIndex`: gitdll `git_update_index()`, or `git.exe update-index --refresh` when gitdll is off or the repository uses LFS. [RVD:115; GIT:2756-2775]
- **Load.** `GetStatus(&paths)` with all defaults, so `FILELIST_MODIFY` only: staged and unstaged changes against HEAD, conflicts and unstaged deletions (5.2.2). Unversioned and ignored files are not loaded. [RVD:117; GSLC.h:466-470]
- **Show.** `Show(VERSIONEDBUTNORMALANDEXTERNALSFROMDIFFERENTREPOS | DIRECTFILES | EXTERNALFROMDIFFERENTREPO, check = DIRECTFILES | ADDED)`. The rows are modified, added and copied, deleted, renamed and conflicted entries under the given paths. [RVD:121-123]
- **Initial checks** [GSLC:688-695]:
  - **Added (and copied) files are always checked.**
  - Any file that was passed **explicitly as a file path** is checked.
  - Every other row is **unchecked**. Reverting a folder therefore opens with only its added files checked; the user must tick the modified ones or use "Select/deselect all".
  - When Explorer passes a single file, that file is checked.

#### 6.3.4 What OK does (`RevertProgressCommand`, target `T` = `HEAD`)

The progress window title is `<common root> - Revert - TortoiseGit` ("Revert" [POT:7825]). The TGitCache is blocked for the repository during the run (`CBlockCacheForPath`). [RVPC:36-39]

**Step 1: move working files to the Recycle Bin.**
- **When.** Only when `HKCU\Software\TortoiseGit\RevertWithRecycleBin` is non-zero (default TRUE). [RVPC:54-55]
- **Which files.** Every selected path, **except** added files and, when `RevertRenamedFilesWithRecycleBin` is 0 (default TRUE), renamed files. [RVPC:41-55]
- **How.** `DeleteAllFiles(bTrash = true)`, which affects existing files only, never directories (so not submodules). It uses `SHFileOperation(FO_DELETE)` with `FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NO_CONNECTED_ELEMENTS | FOF_NOERRORUI | FOF_SILENT`. Errors are silent. [TGP:1658-1697]

The list then reports "Command" / "Revert". [RVPC:57]

**Step 2: sort the paths into tasks, by kind** [RVPC:76-114]:

| Path kind (`m_Action`) | Tasks |
|---|---|
| Renamed (`REPLACED`) with an old path, **when a directory with the old name exists** | Error "Revert failed:\nCannot revert renaming of "<new>". A directory with the old name "<old>" exists." The run stops. |
| Renamed, the new path exists, and it is a file | `rm --ignore-unmatch` on the new path, plus `checkout` of the **old** path |
| Renamed, the new path exists, and it is a directory (a submodule) | goes to the move list (step 4) |
| Renamed, the new path does not exist | `rm -f --cached` on the new path, plus `checkout` of the new path |
| Added (`ADDED`; not `COPY`) | `rm -f --cached` (unstage only; the file stays on disk and was not recycled) |
| Everything else: modified, deleted, conflicted, copied | `checkout` of the path |
| additionally, if Deleted (`DELETED`) | `add -f` of the path |

**Step 3: run the tasks in this order.** Each task is a single git call over all its paths via `--pathspec-from-file=<tmp> --pathspec-file-nul` (`--end-of-options` is kept after the pathspec options) [MGTB:80-128]:
1. `git.exe rm -f --cached …` (unstage)
2. `git.exe rm --ignore-unmatch …` (delete rename targets)
3. `git.exe checkout -f --pathspec-from-file=<tmp> --pathspec-file-nul --end-of-options HEAD`
4. `git.exe add -f …` (deleted paths)

[RVPC:59-73, 116-147]

Each path produces one "Reverted" [POT:7851] row. Deleted paths report from the `add` step, and the `rm --ignore-unmatch` step reports nothing. A non-zero exit code stops the run with git's output in the list. [RVPC:116-147; MGTB:112-117]

**Step 4: submodule renames.** For each submodule rename, run `git.exe mv [-f] -- <new> <old>` (`-f` when the names differ only in case), then `git.exe checkout -f --end-of-options HEAD -- <old>`, and report "Reverted". Cancel is checked after each one. [RVPC:149-195]

**Step 5: after success.**
- The paths are queued for an overlay update. [RVPC:220]
- **Submodules.** If any path was a directory (a submodule), the menu button offers "Handle submodules" [POT:5021]. For each such submodule it runs `/command:diff /submodule /startrev:HEAD /endrev:0000000000000000000000000000000000000000 /path:<sub>` (the old path for renames), which opens the submodule diff dialog (5.1). [RVPC:197-218; GitRev.h:101]
- **Effect on the submodule itself.** Reverting a submodule entry only checks out the gitlink. Leaving the submodule's own working tree untouched is inferred from the commands, not stated in source.

**Not established.**
- **Renamed files after recycling.** With the default settings a renamed file's new path is recycled in step 1, before step 2 tests `path.Exists()`. `CTGitPath` caches existence (`m_bExistsKnown`) [TGP:265-277], so which branch runs depends on whether the list entry had already cached its attributes. That was not traced.
- **Copied entries.** A `COPY` entry (new path, not in `HEAD`) goes to `checkout`. What git then does with a path that is not in `HEAD` was not verified.

### 6.4 The progress dialog these commands open (brief)

- **Window.** `CGitProgressDlg` uses `IDD_SVNPROGRESS` [RC:465-478], caption "Progress", resizable. It contains:
  - the notification list
  - a hidden "Show &log..." button, reused as the post-command menu button
  - a multi-line info text
  - a progress bar
  - "OK" and "Cancel" buttons

  [GPD:64]
- **Rows.** One row per notification: action text and repository path. Right-clicking an Added, Reverted or Resolved row offers "Show log" for that path. [GPL:1090-1155]
- **When the command ends** [GPD:255-313]:
  - Cancel is disabled and OK is enabled and focused.
  - The command's post-command entries go into the menu button, with Alt accelerators taken from their mnemonics.
  - **Auto-close.** `HKCU\Software\TortoiseGit\AutoCloseGitProgress` decides whether the dialog closes by itself: 0 = never (default), 1 = when there was no error and there are no post-command entries, 2 = when there was no error. The `/closeonend:<n>` argument overrides it. [GPD:32-46]
- **Overlays.** `CShellUpdater` sets the cache invalidation event, sends the queued paths to TGitCache over its pipe, and calls `SHChangeNotify(SHCNE_UPDATEITEM)` for files. This happens on `Flush()` (Add calls it explicitly) or in the destructor. [SU:32-34, 70-130]
- **Afterwards.** The dialogs are stand-alone, so TortoiseGitProc exits after the progress dialog closes. There is no results list beyond the progress list.

### 6.5 Resolve (`CResolveDlg`, `IDD_RESOLVE`)

#### 6.5.1 Command (`ResolveCommand::Execute`)

- **The dialog.** It opens unless `/noquestion` is given. [RSLC:27-33]
- **After OK** (or with `/noquestion`), if the path list is not empty:
  - **With `/silent`** (used by TortoiseGitMerge's "Mark as resolved", which passes `/closeonend:1 /noquestion /skipcheck /silent` [TMF:2271-2281]):
    1. For each path, run `git.exe add -f -- <path>`. Any failure shows git's output in a message box and returns.
    2. Delete that path's merge temp files.
    3. If `/resolvemsghwnd` was given and `HKCU\Software\TortoiseGit\RefreshFileListAfterResolvingConflict` (default TRUE) is TRUE, post the registered message `GITSLNM_NEEDSREFRESH` to that window.

    There is no progress dialog. [RSLC:38-67]
  - **Otherwise**, the progress dialog (parented to the Explorer window) runs `ResolveProgressCommand(ResolveWith::Current)`. [RSLC:71-77]
- **Pitfall.** With `/noquestion` and no `/silent`, the paths come straight from the command line with no status bits. `ResolveWith::Current` skips every non-`UNMERGED` path [RSPC:55-60], so such a call would report every path as "Skipped". This is inferred from the code and was not observed.
- **Other callers.** After a merge or pull with conflicts, the progress dialog offers "Resolve" (`/command:resolve /path:"<root>"`). [AU:2339, 3303]

#### 6.5.2 Window

`IDD_RESOLVE DIALOGEX 0,0,289,154`, caption **"Resolve"**, resizable. Controls from RC:800-811:

| Control | ID | Label | Rect (DLU) | Anchor |
|---|---|---|---|---|
| File list | `IDC_RESOLVELIST` | – | 7,7,275,107 | stretches |
| Select all | `IDC_SELECTALL` | "Select/deselect &all" (3-state) | 7,119,85,10 | bottom-left |
| Reminder | `IDC_STATIC_REMINDER` | "Reminder: Commit your change after resolve" (static) | 133,119,149,8 | bottom-right |
| OK | `IDOK` | "OK" (default) | 125,133,50,14 | bottom-right |
| Cancel | `IDCANCEL` | "Cancel" | 179,133,50,14 | bottom-right |
| Help | `IDHELP` | "Help" | 232,133,50,14 | bottom-right |

- **Title.** The path is always the **repository root** (`g_Git.m_CurrentDir`), not the given paths. [RSD:67]

#### 6.5.3 The list

- **Init.** Only Extension is visible by default; all columns are available. Container name "ResolveDlg". The context menu is `POPALL ^ (IGNORE | ADD | COMMIT | EXPORT | RESTORE | SAVEAS | PREPAREDIFF | CHANGELISTS)`. It therefore still has "&Edit conflicts", "Resolved" and the two `Resolve conflict using "%s"` items (5.2.8). [RSD:60]
- **Placeholder.** No empty string is set, so an empty list shows "File list is empty" [POT:4501].
- **Load.** `GetStatus(&paths)` with defaults (`FILELIST_MODIFY`, which includes `ls-files -u` conflict marking). [RSD:143]
- **Show.** `Show(CONFLICTED | INEXTERNALS, check = CONFLICTED)`. `SHOWINEXTERNALS` is 0. **The rows are exactly the conflicted (`UNMERGED`) entries under the given paths, and all start checked.** [RSD:145]

#### 6.5.4 What OK does (`ResolveProgressCommand`, `ResolveWith::Current`)

- **Window.** The progress window title is `<common root> - Resolve - TortoiseGit` ("Resolve" [POT:7771]). The TGitCache is blocked for the repository during the run. [RSPC:32, 48]
- **Per path** [RSPC:50-78]:
  - A path without the `UNMERGED` bit is reported as "Skipped" [POT:8772].
  - Every other path goes into one `git.exe add -f --pathspec-from-file=<tmp> --pathspec-file-nul` call, and its merge temp files are deleted first: `<path>.LOCAL<ext>`, `<path>.REMOTE<ext>` and `<path>.BASE<ext>` [AU:1682-1695].
  - Each added path is reported as "Resolved" [POT:7784].
- **Every conflict kind is handled the same way** (both modified, deleted by us or by them, added by both, submodule). The dialog only marks the working-tree state as resolved; it never picks a side. What `git add -f` does for a conflict whose file is missing from the working tree was not verified.
- **After success** [RSPC:35-46, 320-324]:
  - The menu button offers "&Commit..." [POT:431], which runs `/command:commit /path:"<repository root>"`.
  - The info text reads "You need commit your change after resolve conflict" (hard-coded English).
  - The paths are queued for an overlay update.

#### 6.5.5 "Resolve using theirs / mine" (list context menu only, not the dialog's OK)

The same progress command, with `ResolveWith::Theirs` (stage 3) or `Mine` (stage 2), runs from the list's `Resolve conflict using "%s"` items after the confirmation [POT:1838]. While a rebase is active, theirs and mine are swapped. The progress dialog auto-closes when there are no errors. [GSLC:2462-2490]

**Reading the conflict.** For each path, the stage 1, 2 and 3 entries are read with libgit2 `git_index_conflict_get` (default, `GIT_CMD_GETCONFLICTINFO`), or else from `git.exe ls-files -u -t -z -- <path>`. A path with no conflict is reported as "Skipped". [RSPC:80-164]

**Picking the action:**

| Chosen side | Action |
|---|---|
| Side is absent (deleted there), path is not a directory | `rm -f` task |
| Side is absent, path is a directory | `git.exe rm -f -- <path>`. If that fails and the directory is not empty, ask "Do you really want to delete "%s"?" [POT:3743] with "&Delete" [POT:479] / "A&bort" [POT:1481]. On Delete, remove the directory and retry. |
| Side is a submodule, and the submodule is not initialized | `git.exe checkout-index -f --stage=N -- <path>` (if no directory exists), then `git.exe update-index --replace --cacheinfo 0160000,<hash>,<path>` |
| Side is a submodule, and the submodule is initialized | If the submodule's `HEAD` differs from the chosen hash, open the Reset dialog (`CAppUtils::GitReset`) inside the submodule (cancel aborts), then `add -f` |
| Side is a file | `checkout-index -f --stage=N` task, then `add -f` task |

[RSPC:166-296]

**Order.** The batched tasks run in the order `rm -f`, then `checkout-index`, then `add -f`. Cancel is ignored in this mode. [RSPC:298-313]

### 6.6 Not established from source

- **Add of a deleted path.** Explorer offers Add on deleted items (`DELETED !FOLD !ONE`, 4.2 row 48). A deleted path is not a directory, so it goes straight to `git_index_add_bypath`. Whether that stages the deletion or fails was not checked.
- **Help.** The Help handlers and help topics of the three dialogs.
- **Renamed and copied paths in Revert.** See the "Not established" notes at the end of 6.3.4.
- **Progress bar.** How it behaves for Revert, which sets the total to twice the path count [RVPC:41], was not examined.

## 7. Remotes: Clone, Fetch, Pull, Push (and Sync, briefly)

All four commands follow the same pattern. A dialog collects options. Code in `CAppUtils` (or in the command class, for Clone) builds a single `git.exe` command line. A **console-output progress dialog** (`CProgressDlg`, `IDD_GITPROGRESS`, 7.6) then runs it. This is not the notification-list dialog of 6.4. libgit2 implementations of clone and fetch exist, but they are off by default (7.1.4, 7.2.6). Git never gets a console. Credentials, passphrases and host-key questions reach the user through askpass programs and TortoiseGitPlink, which TortoiseGit installs through environment variables (7.5).

New citation keys (same commit as section 0):

| Key | File |
|---|---|
| **CLD** | `src/TortoiseProc/CloneDlg.cpp` |
| **CLC** | `src/TortoiseProc/Commands/CloneCommand.cpp` |
| **PFD** | `src/TortoiseProc/PullFetchDlg.cpp` |
| **PLC** / **FTC** / **PSC** / **SYC** | `src/TortoiseProc/Commands/PullCommand.cpp` / `FetchCommand.cpp` / `PushCommand.cpp` / `SyncCommand.cpp` |
| **PSD** | `src/TortoiseProc/PushDlg.cpp` |
| **SYD** | `src/TortoiseProc/SyncDlg.cpp` |
| **PD** | `src/TortoiseProc/ProgressDlg.cpp` (`CProgressDlg`, the console-output progress dialog) |
| **GCOP** | `src/TortoiseProc/GitCliOutputParser.cpp` (`\r`/`\n` line handling for `PD`) |
| **HC** | `src/Utils/MiscUI/HistoryCombo.cpp` |
| **SAP** / **SAP.rc** | `src/SshAskPass/SshAskPass.cpp` / `SshAskPass.rc` |
| **TPW** | `src/TortoisePlink/Windows/console.c` (TortoiseGitPlink GUI prompts) |
| **TPC** | `src/TortoisePlink/console.c` (button wording for PuTTY's dialog text) |
| **TPLD** | `src/TortoisePlink/LoginDialog.cpp` |
| **TPRC** | `src/TortoisePlink/Windows/TortoisePlink.rc` |
| **TPSSH** | `src/TortoisePlink/ssh/common.c` (PuTTY host-key dialog text) |
| **FSWA** | `src/TortoiseProc/FirstStartWizardAuthentication.cpp` |

### 7.0 Shared conventions

- **Quoting** (`CGit::QuoteParameter`) [GIT:4015-4034]:
  - Normal mode replaces every `\` with `/` and wraps the value in double quotes. It refuses any value containing `"`, `?` or `*` with the error "The git.exe command could not be constructed. …"; the command then shows that error and stops.
  - "Relaxed" mode (used for refspecs and branch arguments) applies plain Windows argument escaping instead.
  - With MSYS2 or Cygwin git, values are single-quoted for bash.
- **Process.** `RunAsync` gives git a stdin pipe and a single pipe for **stdout and stderr together**, hides the window, and starts git with `DETACHED_PROCESS`, so git has no console. [GIT:272-319] Prompts can only come through askpass (7.5).
- **Per-repository registry keys** use the working directory with `:` replaced by `_` (shown as `<wd>` below). [PFD:125-126; PSD:164-165]
- **History combos.** `LoadHistory` reads `<section>\<prefix>0..n` and then leaves **no item selected**. `AddString` puts a new string first (or re-selects an existing one) and selects it. [HC:144-193, 262-283]
- **Remote settings.** "Manage Remotes" and "Mana&ge" open Settings on the `gitremote` page, then refresh the dialog. [AU:696-707; PFD:467-471; PSD:501-505]
- **Rebase lock.** Pull first checks for `<gitdir>/tgitrebase.active`. If it exists, the user is asked to remove the stale lock or abort [POT:4724]. [AU:2438, 3771-3786]
- **Several paths.** When Pull, Fetch or Push receive more than one path, the command runs once per path that is a working-tree root or a bare repository. Any other path makes the overall result false. [PLC:25-45; FTC:24-44; PSC:25-49]
- **Window type.** All three option dialogs are `CHorizontalResizableStandAloneDialog`s, so they resize **horizontally only**. They save their placement as "CloneDlg", "PullFetchDlg" and "PushDlg". [CLD:32, 193; PFD:33, 265; PSD:37, 157]

### 7.1 Clone (`CCloneDlg`, `IDD_CLONE`)

#### 7.1.1 Window

`IDD_CLONE DIALOGEX 0,0,359,218`, caption **"Git clone - TortoiseGit"**. Controls from RC:1098-1137:

| Control | ID | Label | Rect | Default |
|---|---|---|---|---|
| Group | `IDC_GROUP_CLONE` | "Clone Existing Repository" | 6,10,346,95 | |
| URL | `IDC_URLCOMBO` | "&URL:" (editable, sorted history combo) | 64,25,206,164 | see 7.1.2 |
| URL browse | `IDC_CLONE_BROWSE_URL` | "&Browse..." (split button) | 276,25,67,15 | last entry used |
| Directory | `IDC_CLONE_DIR` | "&Directory:" (edit, filesystem autocomplete) | 64,47,206,14 | see 7.1.2 |
| Dir browse | `IDC_CLONE_DIR_BROWSE` | "Bro&wse..." | 276,46,67,15 | |
| Depth | `IDC_CHECK_DEPTH` + `IDC_EDIT_DEPTH` | "Depth" + number edit | 20,70 / 64,68 | off, 1 |
| Recursive | `IDC_CHECK_RECURSIVE` | "Recursive" | 101,71 | remembered |
| Bare | `IDC_CHECK_BARE` | "Clone into Bare Repo" | 170,71 | off |
| No checkout | `IDC_CHECK_NOCHECKOUT` | "No Checkout" | 276,71 | off |
| Branch | `IDC_CHECK_BRANCH` + `IDC_EDIT_BRANCH` | "Branch" + edit | 20,88 / 64,86 | off, empty |
| Origin | `IDC_CHECK_ORIGIN` + `IDC_EDIT_ORIGIN` | "Origin Name" + edit | 150,88 / 216,86 | off, empty |
| PuTTY key | `IDC_PUTTYKEY_AUTOLOAD` + `IDC_PUTTYKEYFILE` + `IDC_PUTTYKEYFILE_BROWSE` | "Load Putty &Key" + path history combo + "..." | 20,111 / 103,110 / 304,109 | see 7.5.4 |
| SVN group | `IDC_CLONE_GROUP_SVN`, `IDC_CHECK_SVN` … | "From SVN Repository", "From &SVN Repository", "&Trunk:", "Ta&gs:", "Branc&h:", "&From:", "User&name:" | 6,126,346,64 | off; "trunk", "tags", "branches", 0 |
| Buttons | `IDOK`/`IDCANCEL`/`IDHELP` | "OK"/"Cancel"/"Help" | y=196 | |

[RC:1098-1137; CLD:34-61]

- **Tooltips.** Depth [POT:3206], Recursive [POT:9057], Bare [POT:2607], No Checkout [POT:6387], Branch [POT:5635], Origin Name [POT:5639]. [CLD:130-140]
- **Anchoring.** The URL and directory fields, the key combo and both group boxes stretch horizontally. [CLD:116-128]
- **Not in this dialog.** There is **no LFS option** at this commit. Nothing in CLD or CLC mentions LFS.

#### 7.1.2 Defaults and auto-fill

- **Directory.** It starts as the folder TortoiseGitProc was given (the Explorer folder), or the CWD when no path was given. [CLC:76-88] With `/hasurlhandler` there is no path. The dialog then falls back to the **parent of the last clone directory** (`HKCU\Software\TortoiseGit\TortoiseProc\CloneDir`), and then to the user's Documents folder. [CLD:144-156]
- **URL.**
  - `/url:<x>` fills the field. [CLC:90-91; CLD:174-175]
  - Otherwise the **clipboard** is used when it holds a URL. `GetClipboardLink("git clone ")` takes the first line and strips surrounding quotes. It accepts text that starts with `http://`, `https://`, `git://`, `ssh://` or `git@`, or a drive-letter path. It also accepts `git clone <url>` with the prefix stripped. [CLD:163-173; AU:2018-2082]
  - Otherwise the first history entry is used (`HKCU\Software\TortoiseGit\History\repoURLS`, keys `url0…`). [CLD:162]
- **Directory derived from URL** (on every edit of the URL, `CBN_EDITCHANGE`; disabled by `/exactpath`) [CLD:332-402]:
  1. Change `\` to `/` and trim trailing `/`.
  2. The module name is the text after the last `/`. If there is no `/`, it is the text after the last `:`, then after the last `@`, then the whole string.
  3. A trailing `.git` (any case) is dropped.
  4. If the directory currently **ends with the previous module name**, that suffix is removed.
  5. Trailing `\`/`/` are trimmed, and `\<module>` is appended.

  Example: directory `D:\src`, URL `https://host/team/proj.git` gives `D:\src\proj`. Editing the URL to `…/other` gives `D:\src\other`.
- **Initial URL and the directory.** Whether the programmatic URL set at init also fires `CBN_EDITCHANGE` (and so derives the directory) was not established from source.
- **Key autoload follows the URL.** `http://`, `https://` and `git://` URLs untick "Load Putty Key". Any other URL sets it to `CloneUseSSHKey` (default TRUE) and only when the SSH client is plink. [CLD:394-399]
- **URL "&Browse..." split button.**
  - Entry "&Browse..." [POT:387] opens a folder picker that writes into the **URL** field (for local repositories).
  - Entry "&Open" [POT:922] opens the URL in the browser or Explorer.
  - The last entry used is remembered (`CloneBrowse`). [CLD:181-183, 268-297]
- **Mutual exclusions** [CLD:404-437, 467-511]:
  - Recursive or No Checkout disables Bare and clears it.
  - Bare clears and disables Recursive and No Checkout, and disables Origin.
  - Origin disables Bare.
  - "From SVN Repository" clears and disables Depth, Bare, Recursive, Branch and No Checkout. It sets Trunk, Tags and Branch on, unless the URL ends in `trunk`.
- **OK validation** [CLD:225-261]:
  - The URL or the directory is empty: "URL and directory must not be empty." [POT:9808].
  - Branch is on and the name is invalid: a balloon [POT:2162].
  - Origin is on, the name is empty, and this is not an SVN clone: a balloon.
  - **On success** the URL and key histories are saved, along with `CloneDir`, `CloneUseSSHKey` and `CloneRecursive`.

#### 7.1.3 Command line (default, git.exe)

```
git.exe clone --progress[ --recursive][ --bare][ --no-checkout][ --branch "<b>"][ --origin "<o>"][ --depth <n>] -v -- "<url>" "<dir>"
```

[CLC:97-152]

- **URL.** A UNC URL starting with `\\` has its backslashes turned into `/` (only after the `@` when there is one). [CLC:127-141]
- **Origin** is ignored when cloning from SVN.
- **Working directory.** Git runs in the deepest existing ancestor of `<dir>` (or `%TEMP%`). [CLC:32-50, 275]
- **SVN** (one line): `git.exe svn clone[ --prefix "<o>/"][ -T "<trunk>"][ -b "<branches>"][ -t "<tags>"][ -r <from>:HEAD][ --username "<u>"] -- "<url>" "<dir>"`. A local path is turned into a `file:///` URL first. [CLC:188-242]
- **libgit2 variant.** When `GIT_CMD_CLONE` is in `UseLibgit2_mask` (it is **not** in the default mask [GIT.h:34, 307-309]), `CloneProgressCommand` runs in the notification-list progress dialog instead. [CLC:245-269]

#### 7.1.4 After the command

- **Before the run.** If "Load Putty Key" is on, pageant is started with the chosen key file (7.5.4). [CLC:121-122]
- **Failure.** The menu button offers only "&Retry" [POT:1082], which re-opens the progress dialog with the same command. [CLC:160-166, 272-283]
- **Success** [CLC:169-185]:
  - **Bare:** `SetupBareRepoIcon(dir)`.
  - **Key:** if the key was used, `remote.<origin or "origin">.puttykeyfile = <key>` is written into the new repository's config with libgit2. [CLC:52-72]
  - **Menu entries:** "Show &log" [POT:8440] (`/command:log /path:<dir>`) and "Explore to" [POT:4259].

### 7.2 Fetch and Pull (`CPullFetchDlg`, `IDD_PULLFETCH`)

One template serves both commands: `m_IsPull` is TRUE for Pull and FALSE for Fetch. [PFD:37; AU:2442, 2729]

#### 7.2.1 Window

`IDD_PULLFETCH DIALOGEX 0,0,327,223`, caption "Pull/Fetch". The title is replaced by `<wd> - Pull - TortoiseGit` ("Pull" [POT:7249]) or `<wd> - Fetch - TortoiseGit` ("Fetch" [POT:4430]). [RC:1154-1184; PFD:255-261]

| Control | ID | Label | Default |
|---|---|---|---|
| Group | `IDC_GROUPT_REMOTE` | "Remote" | |
| Radio | `IDC_REMOTE_RD` + `IDC_REMOTE_COMBO` | "&Remote:" + drop-down list of remotes | selected |
| Radio | `IDC_OTHER_RD` + `IDC_OTHER` | "Arbitrary &URL:" + editable URL history combo | disabled |
| Remote branch | `IDC_REMOTE_BRANCH` + `IDC_BUTTON_BROWSE_REF` | "Remote &Branch:" + editable combo + "..." | see 7.2.2 |
| Group | `IDC_GROUP_OPTION` | "Options" | |
| | `IDC_CHECK_SQUASH` | "&Squash" | off (Pull only) |
| | `IDC_CHECK_NOFF` | "No &Fast Forward" | off (Pull only) |
| | `IDC_CHECK_NOCOMMIT` | "No Co&mmit" | off (Pull only) |
| | `IDC_CHECK_DEPTH` + `IDC_EDIT_DEPTH` | "Depth" + number | shown only in shallow repositories, then on, 1 |
| | `IDC_CHECK_FFONLY` | "Fast Forward O&nly" | remembered (Pull only) |
| | `IDC_CHECK_FETCHTAGS` + `IDC_STATIC_TAGOPT` | "Tags" (3-state) + config hint | indeterminate |
| | `IDC_CHECK_PRUNE` + `IDC_STATIC_PRUNE` | "Prune" (3-state) + config hint | indeterminate |
| | `IDC_PUTTYKEY_AUTOLOAD` | "AutoLoad Putty &Key" | remembered; enabled only with plink |
| Link | `IDC_REMOTE_MANAGE` | "Manage Remotes" (static hyperlink) | |
| | `IDC_CHECK_REBASE` | "&Launch Rebase After Fetch" | remembered; disabled in bare repositories |
| Buttons | | "OK" / "Cancel" / "Help" | |

[RC:1154-1184; PFD:35-54, 92-269]

- **Fetch mode.** Squash, No Fast Forward, Fast Forward Only, No Commit and the Options group box are **disabled**. [PFD:222-229]
- **No Fast Forward and Fast Forward Only** exclude each other. Ticking one clears and disables the other. [PFD:496-514]
- **Not in this dialog.** There is **no** autostash, `--rebase` (as a pull flag), `--recurse-submodules` or verbose option.

#### 7.2.2 Remote and remote branch selection (`Refresh`)

[PFD:271-334]

- **Pull remote.** `branch.<current>.remote`. If that is empty, the last remote used here (`HKCU\Software\TortoiseGit\History\PullRemote\<wd>`). A remote given by the caller (`/remote:` for Fetch, `m_PreSelectRemote`) overrides both. [PFD:278-315; FTC:44]
- **Remote list.** `git remote` order.
  - **Fetch with more than one remote** puts "- all -" [POT:1365] first. When no remote matches, index 0 is selected, which in fetch mode with several remotes **is "- all -"**. [PFD:317-332]
  - **Pull** never offers "- all -".
- **Remote branch combo.**
  1. It is loaded from `History\PullRemoteBranch` and the first entry is selected.
  2. `Refresh` then adds and selects one of these, in order: the tracked branch (`branch.<current>.merge` without `refs/heads/`); for a submodule without one, the parent's `submodule.<path>.branch`; otherwise the current branch name.

  [PFD:252-253, 283-309; GIT:845-857]
- **Fetch with a named remote.** `HKCU\Software\TortoiseGit\NamedRemoteFetchAll` (default TRUE) **disables the remote-branch combo and "..."**. [PFD:52, 210-214]
- **Config hints next to the 3-state boxes** (for a named remote) [PFD:337-371; POT:3340, 6493, 1661, 7394]:
  - Tags: "Default: None|All|Reachable", from `remote.<r>.tagopt`.
  - Prune: "Default: <value>", from `remote.<r>.prune`, else `fetch.prune`. Empty when neither is set.
- **Switching to "Arbitrary URL"** [PFD:388-423]:
  - The clipboard is checked for `git pull <url> <branch>` or `git fetch <url> <branch>` (or a bare URL). This fills the URL and the branch.
  - It **disables and clears "Launch Rebase After Fetch"**.
  - In Fetch mode it re-enables the branch combo.

  Switching back to "&Remote:" resets the rebase box to the config-forced value (7.2.3). [PFD:373-387]
- **"..."** opens `CSelectRemoteRefDlg` for the selected remote or URL and adds the chosen branch. [PFD:473-488]

#### 7.2.3 Rebase forced by config (Pull only)

The dialog reads `branch.<current>.rebase`, falling back to `pull.rebase`, unless HEAD is detached. If the value is true or `merges`, "Launch Rebase After Fetch" is ticked and `m_bRebaseActivatedInConfigForPull` is set (`merges` also sets preserve-merges). The comment explains why: plain `git pull` would rebase anyway. [PFD:148-197]

#### 7.2.4 OK and what is remembered

[PFD:426-465]

- **Remote radio.**
  - `m_RemoteURL` = the remote name; "- all -" means all remotes.
  - The remote branch is **left out** in two cases, so that git updates the remote-tracking refs: Fetch with `NamedRemoteFetchAll`, and Pull when the remote and branch equal the configured upstream.
- **URL radio.** The URL and branch are used as typed, and the URL is saved to `History\PullURLS`.
- **Rebase with no branch.** With rebase on, Pull and an empty branch field: "You asked to pull, but did not specify a remote branch." [POT:10531].
- **Saved:**
  - `History\PullRemote\<wd>` and the branch history.
  - `HKCU\Software\TortoiseGit\TortoiseProc\PullFetch\<wd>_<isPull>\{rebase, ffonly, autoload}`. The defaults are false, false and "SSH client is plink".

  These keys are **separate for Pull (`_1`) and Fetch (`_0`)**. Squash, No-FF, No Commit, Tags, Prune and Depth are **not** remembered. [PFD:128-137]

#### 7.2.5 Pull command (`DoPull`)

If "Launch Rebase After Fetch" is ticked, Pull runs **Fetch + Rebase** instead (7.2.6), because TortoiseGit never uses `git pull --rebase`. [AU:2445-2457]

```
git.exe pull --progress -v --no-rebase[ --no-tags|--tags][ --no-ff][ --ff-only][ --squash][ --no-commit][ --depth <n>][ --prune|--no-prune][ --allow-unrelated-histories] -- "<remote|url>"[ <branch>]
```

[AU:2282-2317]

- **Tags.** Unchecked gives `--no-tags`, checked gives `--tags`, indeterminate adds nothing. Prune maps the same way.
- **Depth** is passed only when the box is on.
- **The branch** uses relaxed quoting.
- **`--allow-unrelated-histories`** is added only by the "Merge unrelated history" retry.
- **Before the run.** The PuTTY key is loaded first when ticked, and HEAD is recorded as `hashOld`. [AU:2272-2280]

**Post-command entries** [AU:2326-2420]:

| Situation | Entries |
|---|---|
| Failure with conflicts | Info box [POT:10419] (with a "do not show again" box, value `MergeConflictsNeedsCommit`), then "Resolve" [POT:7771] (`/command:resolve`) and "&Commit..." [POT:431] |
| Failure without conflicts | First, when the remote is a named remote and HEAD and `remotes/<r>/<b>` have **no common ancestor**: "&Merge unrelated history" [POT:838]. Then always: "&Pull..." [POT:994], "Stash changes" [POT:8924], and "Reset" [POT:7755], which opens the Reset dialog on `remotes/<upstream>` with type 2 (hard, judging by the order of the radio buttons) |
| Success | "Stash Pop" (only when the caller asked for it), "Pulled Diff" [POT:7265] (`/command:showcompare /revision1:<old> /revision2:<new>`), "Pulled Log" [POT:7269] (`/command:log /range:<old>..<new>`), "Pus&h..." (when the caller asked for it, e.g. Pull started from a rejected push), and "Update Submodules" [POT:9979] (`/command:subupdate /bkpath:`) when the repository has submodules |

**After the dialog closes** with OK, if git exited with 1 and the output contains `CONFLICT`, the user is asked "Do you want to see changes?" [POT:3836]. Yes opens `/command:repostatus`. [AU:2424-2431]

#### 7.2.6 Fetch command (`DoFetch`)

```
git.exe fetch --progress[ -v][ --depth <n>][ --prune|--no-prune][ --tags|--no-tags] -- "<remote|url>"[ <refspec>]
git.exe fetch --all --progress[ -v][ …same options…]          (for "- all -")
```

[AU:2570-2605]

- **`-v`** is controlled by `HKCU\Software\TortoiseGit\FetchVerbose` (default TRUE).
- **The 3-state mappings** are those of 7.2.5.
- **The PuTTY key** is loaded for the chosen remote, or for every remote with "- all -". [AU:2523-2535]
- **libgit2.** `FetchProgressCommand` is used only when `GIT_CMD_FETCH` is in the mask; the default is git.exe. [AU:2650-2663; GIT.h:34]

**Post-command entries** [AU:2608-2646]:

| Situation | Entries |
|---|---|
| Failure | "&Retry" (and "Show &log" for all remotes) |
| Success | "Show &log", "Reset" (as for Pull), "F&etch..." [POT:4320], "Rebase..." [POT:7415] (only when no rebase was requested and the repository is not bare), "S&witch/Checkout..." [POT:7962] |

**Rebase after fetch** (the Pull checkbox, or the Fetch checkbox) runs in `m_PostExecCallback` when the exit code is 0 [AU:2537-2568, 2665-2720]:

1. **Upstream.** For a named remote: `remotes/<r>/<branch>`, or `remotes/<r>/<tracked branch>` when the branch was omitted. For an arbitrary URL: `FETCH_HEAD`.
2. **Checks** (only when the user asked for the rebase, not when the config forced it):
   - **HEAD already contains the upstream:** "Current branch is up to date or newer than the fetched branch. Open rebase anyway?" [POT:3287].
   - **Upstream did not move:** "The remote branch has not changed.\n\nOpen the rebase dialog anyway?" [POT:9443].
   - **HEAD can fast-forward** to it: "The fetched branch fast-forwards upon the current branch.\n\nMerge or open the rebase dialog anyway?" [POT:9294], with "&Merge" / "&Rebase" / "A&bort". Merge runs `git.exe merge --ff-only -- "<upstream>"` in a progress dialog that auto-closes when there are no errors.

   Each question has a "do not show again" checkbox.
3. **Otherwise** `CRebaseDlg` opens with that upstream. It **auto-starts** when the rebase was forced by config. Its post buttons are "Show &log", "Pus&h...", "Send Patch(es) by eMail" [POT:8356] and "Rebase..." (which loops back). [AU:2465-2519]

### 7.3 Push (`CPushDlg`, `IDD_PUSH`)

#### 7.3.1 Window

`IDD_PUSH DIALOGEX 0,0,311,277`, caption "Push". The title becomes `<wd> - Push - TortoiseGit`. [RC:1186-1223; PSD:153]

| Group | Control | Label | Default |
|---|---|---|---|
| "Ref" | `IDC_PUSHALL` | "&Push all branches" | remembered per repository (only when no branch was preselected) |
| | `IDC_BRANCH_SOURCE` + `IDC_BUTTON_BROWSE_SOURCE_BRANCH` | "&Local:" + editable combo of local branches + menu button ("Reference Browser" / "Log" / "RefLog") | current branch |
| | `IDC_BRANCH_REMOTE` + `IDC_BUTTON_BROWSE_DEST_BRANCH` | "&Remote:" + editable combo + "..." (remote ref picker; also selects the remote) | push branch |
| "Destination" | `IDC_RD_REMOTE` + `IDC_REMOTE` + `IDC_REMOTE_MANAGE` | "Re&mote:" + drop-down list ("- all -" first when there is more than one remote) + "Mana&ge" | see 7.3.2 |
| | `IDC_RD_URL` + `IDC_URL` | "Arbitrary &URL:" + URL history (`History\PushURLS\<wd>`) | disabled |
| "Options" | `IDC_FORCE_WITH_LEASE` | "Force &with lease" | off |
| | `IDC_FORCE` | "&Force" | off |
| | `IDC_TAGS` | "Include &Tags" | off |
| | `IDC_PUTTYKEY_AUTOLOAD` | "&Autoload Putty Key" | remembered (`History\PushDlgAutoLoad\<wd>`, default "plink in use"); enabled only with plink |
| | `IDC_PROC_PUSH_SET_UPSTREAM` | "&Set upstream/track remote branch" | on if HEAD's branch has no upstream |
| | `IDC_PROC_PUSH_SET_PUSHREMOTE` | "Always push to the selected remote archive for this local branch" | off |
| | `IDC_PROC_PUSH_SET_PUSHBRANCH` | "Always push to the selected remote branch for this local branch" | off |
| | `IDC_COMBOBOX_RECURSE_SUBMODULES` | "Recurse submodule": "None" / "Check" / "On demand" | 7.3.3 |
| | `IDC_PUSHOPTION` | "Push &option:" (history `History\PushOption\<wd>`) | empty |

[RC:1186-1223; PSD:39-52, 96-216]

- **Tooltips.** Force [POT:4691], Force with lease [POT:4687], pushremote [POT:8380], pushbranch [POT:8384]. [PSD:190-193]
- **F5** refreshes the dialog. [PSD:577-592]
- **Enable rules** [PSD:336-356, 594-639]:
  - **Force and Include Tags** both disable "Force with lease". "Force with lease" disables both of them.
  - **"Push all branches"** disables the Local and Remote combos and their buttons.
  - **"Set upstream"** clears and disables the two "Always push…" boxes. Either "Always push…" box clears and disables "Set upstream".
  - **The two "Always push…" boxes are disabled and cleared** when "- all -" is selected, "Push all branches" is on, the URL radio is chosen, or Local is empty.
  - **"Set upstream" stays enabled** only for the remote radio with a non-empty Local (or "Push all branches").

#### 7.3.2 Default remote and remote branch

1. **Local.** A preselected `refs/heads/x` or `remotes/…` (from the log, `/branch:`) is used. Otherwise the current branch, or `HEAD` when detached. [PSD:248-272; PSC:27-29]
2. **Remote**, first pass: the last remote pushed to (`History\PushRemote\<wd>`), or "- all -" if that was the last choice. [PSD:223-246]
3. **Then `GetRemotePushBranch(local)`** runs [PSD:289-334; GIT:867-891]:
   - **Remote:** `branch.<local>.pushremote`, else `remote.pushdefault`, else `branch.<local>.remote`. If one is found it is selected (and written to the `PushRemote` history at once).
   - **Without one:** the only remote is selected if there is just one. Otherwise **nothing is selected** (unless "- all -" is), and OK then says "Remote name must not be empty." [POT:7589].
   - **Remote branch:** `branch.<local>.pushbranch` (a **TortoiseGit-specific** key, not stripped, for Gerrit), else `branch.<local>.merge` without `refs/heads/`. It is selected over the `History\RemoteBranch\<wd>` entries. With no upstream the field stays **empty**, and git then pushes to the branch of the same name.
4. **Not consulted.** `push.default` is not read anywhere in this path.
5. **Re-run on change.** Changing Local re-runs step 3. [PSD:401-404]
6. **All remotes.** `TortoiseProc\Push\<wd>\AllRemotes` reselects "- all -". [PSD:166-167, 284-286]
7. **"Arbitrary URL"** fills itself from a `git pull`/`git fetch` clipboard line, like Pull. [PSD:360-398]

#### 7.3.3 OK

[PSD:406-499]

- **"Push all branches":** "Do you really want to push all local branches?" [POT:3773], with a "do not show again" box.
- **Otherwise**, in order:
  1. Local and Remote both empty: "…are empty.\nContinue?" [POT:9391].
  2. Local empty but Remote set: "This results in a remote removal.\nContinue?" [POT:9395].
  3. Remote name invalid: [POT:2158].
  4. An ambiguous source ref: [POT:9290].
- **"Always push…" boxes** set or unset `branch.<local>.pushbranch` / `branch.<local>.pushremote` in the repository config. They apply only to local branches.
- **Saved:** the remote, URL, branch and push-option histories; `AllBranches`, `AllRemotes`, autoload; and the recurse choice (`History\PushRecurseSubmodules\<wd>`, whose initial default comes from `push.recurseSubmodules`). [PSD:195-207]

#### 7.3.4 Command lines (`DoPush`)

[AU:2738-2862]

- **Pre-push hook.** A TortoiseGit pre-push client hook runs first; on failure a task dialog offers to abort or continue. [AU:2742-2759]
- **Options string** (the order is fixed): `[--tags ][--force ][--force-with-lease ][--set-upstream ][--recurse-submodules=no|check|on-demand ][--push-option=<opt> ]--progress `.
  - `--tags` is added only when "Push all branches" is off.
  - `--recurse-submodules=…` is added **only when the choice differs from `push.recurseSubmodules`**.
- **The command, once per remote** ("- all -" means every remote; the PuTTY key is loaded for each):

```
git.exe push <options> -- "<remote|url>" [<local>[:<remoteBranch>]]          (single branch; refspec relaxed-quoted)
git.exe push --all <options> -- "<remote>"                                   (push all branches)
git.exe push --tags <options> -- "<remote>"                                  (extra command when all branches + tags)
git.exe rev-list --count --first-parent --end-of-options "<local>" --        (only if ShowBranchRevisionNumber)
```

- **The run.** All commands go into one progress dialog. A failing command **does not stop the later ones**; the exit codes are OR-ed. [PD:228-273]

**Post-command entries** [AU:2866-2908]:
- **The post-push hook** runs first.
- **Failure:**
  - If the output contains `! [rejected]`: "&Pull..." (a Pull that then offers Push) and "F&etch..." (same remote).
  - Always: "Pus&h...".
- **Success:**
  - "Create pull &request" [POT:3214], which opens `CRequestPullDlg`, then `git.exe request-pull -- <start> <url> <end>`. [AU:2926-2940]
  - "Pus&h...".
  - "S&witch/Checkout...".
  - "Commit super project" [POT:2742] when the repository is a submodule.

### 7.4 Sync (`CSyncDlg`, `IDD_SYNC`), summary only

"Git Synchronization" (`/command:sync`) is a resizable all-in-one window [RC:1679-1710; SYC:25-34]:
- **Top area:** "&Local Branch:", "&Remote Branch:" and "Remote &URL:" combos, "Manage", "Autoload Putty &Key" and "&Force".
- **Middle:** a tab control with "In Commits", "In Changelist", "Conflicts", "Out Commits", "Out Changelist", "Compare tags", and an embedded git output / progress view. [SYD:1038-1133]
- **Bottom:** menu buttons.

It runs the same kinds of git commands itself, with its own simpler options (no tags, prune or depth choices). The status line reports ahead and behind counts. [SYD:1379-1450] Its operations:
- **Pull button**:
  - "&Pull": `git.exe pull -v --progress[ --force] -- <url> [<branch>]` (the branch is omitted when it is the upstream).
  - "Fetc&h": `git.exe fetch --progress[ -v][ --force] -- <url> [<branch>:remotes/<url>/<branch>]`.
  - "Fetch && Re&base".
  - "Fetch all refs" (fetch with no branch).
  - "Remote Update": `git.exe remote update`.
  - "Clean up stale remote branches": `git.exe remote prune -- <url>`.
  - "Compare tags".

  [SYD:375-500, 1178-1184]
- **Push button:**
  - "Pus&h": `git.exe push -v --progress[ --force] -- <url> <local>[:<remote>]`.
  - "Push ta&gs" (adds `--tags`).
  - "Push notes" (pushes the notes ref).

  [SYD:777-810, 1174-1176]
- **Submodule button:** "Submodule Update" (`submodule update --init --recursive`), "Submodule Init", "Submodule Sync" (`--recursive`). [SYD:1773-1790]
- **Stash button:** "Stash changes" (`stash save`), "Stash Pop", "Stash Apply". [SYD:1823-1837]
- **Other buttons:** "&Apply Patch" (`git.exe am`), "&Email Patch" (`format-patch`), "Show &log", "&Commit", "Close". [SYD:839, 915]

### 7.5 Credentials and prompts

#### 7.5.1 Environment for every git process (`CGit::CheckMsysGitDir`)

The environment is built once from the process environment. The settings below apply to every git TortoiseGit runs. [GIT:2347-2435]

| Variable | Value |
|---|---|
| `TGIT_INITIATED_CALL` | `1` |
| `GIT_INDEX_FILE`, `GIT_DIR`, `GIT_WORK_TREE`, `GIT_CONFIG*`, `GIT_CEILING_DIRECTORIES` and similar | **removed** (sanitised) |
| `LC_ALL` | `C` if unset |
| `HOME` | set to TortoiseGit's home directory if unset |
| `GIT_SSH` / `SVN_SSH` | `HKCU` (then `HKLM`) `Software\TortoiseGit\SSH` if set; otherwise `<TortoiseGit bin>\TortoiseGitPlink.exe` |
| `GIT_SSH_VARIANT` | `ssh`, when the client is TortoiseGitPlink (by default, or when the configured path ends in `tortoisegitplink[.exe]`); for any other client it is left as is |
| `DISPLAY` | `:9999` (OpenSSH only uses `SSH_ASKPASS` when `DISPLAY` is set) |
| `SSH_ASKPASS`, `GIT_ASKPASS`, `GIT_ASK_YESNO` | `<TortoiseGit bin>\SshAskPass.exe` |
| `SSH_ASKPASS_REQUIRE` | `force` (Win32-OpenSSH compatibility, issue #3996) |

- **Not set:** `GIT_TERMINAL_PROMPT` and credential-helper variables. HTTP credentials therefore come from git's own `credential.helper` (the first-start wizard may write one into the global or XDG config [FSWA:102-123]). When no helper answers, git falls back to `GIT_ASKPASS`, i.e. SshAskPass.
- **SSH client choice.** The first-start wizard sets `SSH` to `TortoiseGitPlink.exe` or `ssh.exe`. [FSWA:90-100]
- **Plink check.** `CAppUtils::IsSSHPutty()` is simply "`GIT_SSH` contains `plink`" (case-insensitive). It enables every "Autoload Putty Key" box. [AU:2011-2016]

#### 7.5.2 SshAskPass.exe (`GIT_ASKPASS` / `SSH_ASKPASS` / `GIT_ASK_YESNO`)

The prompt arrives as the command line. One pair of surrounding quotes is stripped. With no argument the prompt is "Enter your OpenSSH passphrase:". [SAP:45-69] The prompt is classified by substring:

1. **Contains `(yes/no`** (OpenSSH host-key question): a task dialog with Yes/No command links. Without task-dialog support, a `MessageBox(MB_YESNO)` titled "TortoiseGit - Git CLI stdin wrapper". The program prints `yes` or `no` to stdout. [SAP:71-87]
2. **Contains `Should I try again?`** (Git for Windows `GIT_ASK_YESNO`, e.g. a failed unlink): the same Yes/No dialog, titled "TortoiseGit - Git CLI yes/no wrapper". The answer is the **exit code**: 0 = yes, 1 = no. [SAP:89-102]
3. **Anything else** (password, passphrase, `Username for '…':`) opens `IDD_ASK_PASSWORD` [SAP.rc:41-50]:
   - **Layout.** The caption is "TortoiseGit -  git CLI stdin wrapper" (two spaces). A static shows the prompt, with an edit 215 DLU wide and OK/Cancel. The dialog is system-modal and centred on the screen.
   - **Sizing.** It grows to fit the prompt text, and it flashes.
   - **Masking.** The edit is **masked only if the prompt contains `pass`** (case-insensitive). Username prompts are therefore visible. [SAP:225-251]
   - **Limits and wiping.** Input is limited to 255 characters. The field is overwritten with `*` and then cleared on close. [SAP:247, 302-319]
   - **OK** prints the text as **UTF-8 plus `\n`** and exits 0. **Cancel** prints `\n` and exits −1. [SAP:104-117]
   - **Theme.** Dark mode follows `HKCU\Software\TortoiseGit\DarkTheme`. [SAP:163-206]

#### 7.5.3 TortoiseGitPlink (PuTTY plink with GUI prompts)

- **Host key.** The unknown or changed host-key text is PuTTY's own: title "TortoiseGitPlink Security Alert", text "The host key is not cached for this server: … The server's <type> key fingerprint is: … Store key in cache?", and for a changed key "WARNING - POTENTIAL SECURITY BREACH!" … "Update cached key?". The action words are adapted to buttons (`hit "Yes"`, `hit "No"`, `hit "Cancel"`). [TPSSH:938-1070; TPC:12-23] The text appears in `MessageBox(MB_ICONWARNING | MB_YESNOCANCEL, default Cancel)`: Yes stores the key and continues, No continues once, Cancel aborts. [TPW:117-140]
- **Weak crypto / weak cached host key.** Yes/No message boxes titled "TortoiseGitPlink Security Alert". [TPW:142-193]
- **Passwords, passphrases and keyboard-interactive prompts.** Each prompt opens `IDD_LOGIN` (caption "TortoiseGitPlink", prompt static, masked edit, OK/Cancel, parented to the desktop). Cancel aborts the connection. [TPW:349-376; TPLD:51-146; TPRC:39-48] So with the default plink client, **SshAskPass is not used for SSH**. It handles HTTP prompts and OpenSSH.
- **Not traced.** Where TortoiseGitPlink stores accepted host keys (PuTTY's `PUTTY_REG_POS` registry tree).

#### 7.5.4 "Autoload Putty Key"

- **When.** Before running the command, if the box is ticked. [CLC:121-122; AU:2272-2273, 2523-2535, 2809-2810]
- **Key lookup** (`CAppUtils::LaunchPAgent(keyfile, remote)`) [AU:614-662]:
  - The key is the explicit file (Clone) or `git config remote.<remote>.puttykeyfile`. The remote defaults to `origin`.
  - With no key it returns at once.
  - The "remote" is the selected remote name. For an arbitrary URL the lookup is `remote.<url>.puttykeyfile`, which normally does not exist.
- **Running pageant:**
  1. Run `"<bin>\pageant.exe" "<key>" -c "<bin>\tgittouch.exe" "<tempfile>"`. Pageant asks for the passphrase itself, then runs `tgittouch` to create the temp file.
  2. Poll every 100 ms for the file, for up to 5 minutes. On timeout: "Failed waiting for pageant to finish loading key." [POT:4405].
  3. If pageant cannot start: "Could not start the Putty Agent (pageant)." [POT:3137].
- **Clone** writes `remote.<origin>.puttykeyfile` after success, so later fetches and pushes find the key (7.1.4).

### 7.6 Progress dialog for these commands (`CProgressDlg`, `IDD_GITPROGRESS`)

#### 7.6.1 Window

Caption "Git Command Progress", resizable, with minimize and maximize boxes. The title becomes `<wd> - Git Command Progress - TortoiseGit`. [RC:1139-1152; PD:169] Controls:
- a download animation, with the current-action label below it
- a smooth progress bar
- a read-only RichEdit log that stretches
- a hidden post-command menu button at the bottom left
- "Close" (`IDOK`) and "Abort" (`IDCANCEL`), both disabled at first

[PD:132-136]

#### 7.6.2 Running

- **Worker thread.** It runs the command list in order. Before each command it echoes the command line into the log (`m_bShowCommand` defaults to true). [PD:48, 198-275]
- **Repository lock.** TGitCache is blocked for the repository during the run.
- **Output** is read in 1 KiB chunks into `CGitCliOutputParser`.
- **Timer.** A 125 ms timer moves the parsed text into the RichEdit. [PD:308, 459-465]

#### 7.6.3 `\r` progress lines

These are handled specially [GCOP:37-127, 141-267; PD:521-606]:
- **Input.** `\0` becomes `\n`. Lines are capped at 8 KiB ("... [line truncated at 8 KiB]") and the buffer at 150 MiB.
- **Pending line.** A line ending in `\r` is held as the **pending CR line** and shown, but it can be replaced.
  - The next `\r` line **overwrites** it. A shorter new text overlays the start of the old one, like a console.
  - The parser tells the UI to erase the previous last line (`erasePreviousLineWithLength`). The UI selects from the start of the last line to the end and replaces it. So `Receiving objects:  45% (…)\r` updates in place.
  - A following `\n` line makes the overlaid text permanent.
- **`remote: ` lines** get extra handling, so that the server's `remote: …\r` progress and the empty `remote: ` lines between them do not leave blank or duplicate rows.
- **Text clean-up.** ANSI `ESC[K` and colour sequences `ESC[…m` are removed (`ClearESC`). [PD:481-519]
- **Progress bar.** In each new text chunk, the label is set to the text before the last `:`, and the bar to the number just before the first `%`. The taskbar button shows the same progress. [PD:587-606]
- **Output limit.** `HKCU\Software\TortoiseGit\GitOutputLimitinKiB` (default 2048, range 16 to 102400). Beyond it the log shows "[Output truncated at about N KiB]" and further output is dropped. [PD:44, 568-584]

#### 7.6.4 End

[PD:311-455]

- **Status line.** "Success" [POT:9061] in bold blue, or "git did not exit cleanly (exit code %d)" [POT:10836] in bold red with a beep (unless `NoSounds`). Timings are added unless `ShowGitexeTimings` = 0.
- **Styling.** Warnings, errors and URLs in the log are styled (`StyleGitOutput`, default TRUE). URLs are clickable.
- **Post-command entries** (only when the command list completed, not on a start failure) fill the menu button. Alt+mnemonic accelerators are built from their labels. Choosing one hides the dialog, runs the action and closes the dialog. [PD:651-656]
- **Auto-close.** `AutoCloseGitProgress` / `/closeonend` work as in 6.4 (1 = no error and no post entries, 2 = no error). [PD:58-73, 449-452]
- **Abort while running** [PD:665-698]:
  1. Ask "The process is still running.\nAre you sure to abort?" [POT:9430], only when `ConfirmKillProcess` is set.
  2. Send Ctrl+C and wait up to 10 s.
  3. Kill the process tree.
- **Log file.** Closing writes the log text to TortoiseGit's log file (`CLogFile`). [PD:610-649]

### 7.7 Not established from source

- Whether setting the Clone URL at init (from `/url` or the clipboard) triggers the directory auto-fill (7.1.2).
- Reset type 2 = hard: inferred from the order of the `ResetDlg` radio buttons, not traced.
- `CSelectRemoteRefDlg` (Pull "...") and `CRequestPullDlg`: their layouts were not examined.
- Where TortoiseGitPlink stores accepted host keys, and how pageant prompts for the passphrase (PuTTY code, not read).
- The Help topics of the three dialogs.
- Sync: only the command lines and menu entries above were read. Its tab contents, ahead/behind logic and post-command flow were not examined.

## 8. Branches: Create Branch/Tag, Switch/Checkout, Merge, Abort Merge

Four commands, three dialogs that share one ref chooser (`CChooseVersion`, 8.1), and one small reset dialog. Create Branch and Create Tag are the **same dialog** (`CCreateBranchTagDlg`) with a flag. Create Branch/Tag runs git synchronously with no progress window. Switch, Merge and Abort Merge run in the console-output progress dialog `CProgressDlg` (7.6), whose post-command buttons are listed per command below.

New citation keys (same commit as section 0):

| Key | File |
|---|---|
| **CV** | `src/TortoiseProc/ChooseVersion.h` (the shared HEAD/Branch/Tag/Commit chooser; all code is in the header) |
| **CBTD** | `src/TortoiseProc/CreateBranchTagDlg.cpp` |
| **GSWD** | `src/TortoiseProc/GitSwitchDlg.cpp` |
| **MGD** | `src/TortoiseProc/MergeDlg.cpp` |
| **MAD** | `src/TortoiseProc/MergeAbortDlg.cpp` |
| **BRC** / **TGC** | `src/TortoiseProc/Commands/BranchCommand.cpp` / `TagCommand.cpp` (`.h` cited as **BRC.h** / **TGC.h**) |
| **SWC** / **MGC** | `src/TortoiseProc/Commands/SwitchCommand.cpp` / `MergeCommand.cpp` |

`<wd>` is the working directory. Quoting (`QuoteParameter`) is as in 7.0. `FixBranchName` replaces the literal `FETCH_HEAD` with the hash it points to and leaves every other name alone. [GIT:1561-1573]

### 8.0 Entry points

| TortoiseGitProc command | Calls | Path requirement |
|---|---|---|
| `branch [/rev:<ref>] [/name:<name>]` | `CreateBranchTag(isTag=false, ref=/rev or null, switch=false, name)` [BRC:26-31] | working tree **or bare repo** [BRC.h:32] |
| `tag [/rev:<ref>] [/name:<name>]` | `CreateBranchTag(isTag=true, …)` [TGC:26-31] | working tree or bare repo [TGC.h:32] |
| `switch [/rev:<ref>]` | `Switch(initialRef=/rev)` [SWC:26-28] | working tree (default, CMDH:96) |
| `merge` | `Merge()` [MGC:29] | working tree |
| `merge /abort` | `MergeAbort()` [MGC:26-27] | working tree |

Explorer rows 35 and 42 to 45 (4.2) pass only `/path`, never `/rev`. Other in-process callers: the Log (8.6), Browse References (Merge, Switch, Create Branch/Tag) [BrowseRefsDlg.cpp:1207-1247], the Revision Graph (`PerformSwitch`/`Switch`) [RevisionGraphWnd.cpp:1086, 1267], the Check-for-modifications branch link (`Switch`) [CHD:323-328], the Commit dialog's detached-HEAD warning (Yes runs `CreateBranchTag(isTag=false, switchNewBranch=true)`) [CD:2960-2966], and the Pull success button "&Merge..." (`Merge(&mergeRev, showStashPop=true)`) [AU:148].

### 8.1 The shared ref chooser (`CChooseVersion`)

A mix-in class that the three dialogs inherit. Its controls are in each dialog's RC, with the same IDs, inside a group box `IDC_GROUP_BASEON`:

| Control | Create Branch/Tag | Switch | Merge |
|---|---|---|---|
| Group caption | "Base On" [RC:1295] | "Switch To" [RC:1264] | "From" [RC:1377] |
| `IDC_RADIO_HEAD` "&HEAD" | yes [RC:1296] | **absent** | **absent** |
| `IDC_RADIO_BRANCH` "&Branch" + `IDC_COMBOBOXEX_BRANCH` (drop-list) + "..." `IDC_BUTTON_BROWSE_REF` | yes [RC:1297-1299] | yes [RC:1265-1267] | yes [RC:1378-1380] |
| `IDC_RADIO_TAGS` "&Tag" + `IDC_COMBOBOXEX_TAGS` (drop-list) | yes [RC:1300-1301] | yes [RC:1268-1269] | yes [RC:1381-1382] |
| `IDC_RADIO_VERSION` "&Commit" + `IDC_COMBOBOXEX_VERSION` (**editable** drop-down) + "..." `IDC_BUTTON_SHOW` | yes [RC:1302-1304] | yes [RC:1270-1272] | yes [RC:1383-1385] |

- **Enabling.** Only the checked radio's combo is enabled. The branch "..." is enabled only with Branch, the commit "..." only with Commit. Every radio change calls the dialog's `OnVersionChanged` hook. [CV:63-92]
- **Loading.** `InitChooseVersion` disables OK and the Branch and Tag radios, then loads refs on a worker thread. [CV:257-284] The thread gets:
  - branches from `GetBranchList`, with `BRANCH_ALL_F` (local + remote + `FETCH_HEAD`) when `HKCU\Software\TortoiseGit\BranchesIncludeFetchHead` is set (default TRUE), else `BRANCH_ALL`; [CV:211]
  - tags from `GetTagList`. [CV:216]

  When the thread posts back, `UpdateGUI` fills both combos and re-enables the radios and OK. [CV:225-256]
- **List contents and order.**
  - Remote branches **are listed**, as `remotes/<remote>/<branch>`. Local branches are bare names. The CLI path skips `<remote>/HEAD -> …` lines and strips the `* ` and `+ ` markers. [GIT:1761-1797] `FETCH_HEAD` is appended only when it resolves. [GIT:1802-1803]
  - Branches sort **local first, then remote** (unless `NoSortLocalBranchesFirst`), each group by `StrCmpLogicalW`. Logical sort is off when the Explorer policy `NoStrCmpLogical` is set, and plain case-insensitive comparison is used instead. [GIT:183-197, 201-226, 1805]
  - Tags sort the same way, and descending when `SortTagsReversed` is set (HKLM or HKCU, default 0). [GIT:195-197, 1492, 1506]
  - The RC also sets `CBS_SORT` on the ComboBoxEx controls. Whether that affects the order was not established; the code relies on its own sort.
- **Default selection after load.**
  - The branch combo selects the **current branch**, unless HEAD is detached or the dialog skips the current branch (Merge does, 8.4). Otherwise it has no selection.
  - The tag combo selects its **first** entry. [CV:229-232; GIT:1807-1817]
  - When the dialog has an `IDC_CURRENTBRANCH` control (Merge only), it shows `GetCurrentBranch()`, or "(no branch)" when detached. [CV:233-234; GIT:835-843]
- **Preselection (`m_initialRefName`).** The caller sets it before `DoModal`; the TortoiseGitProc commands take it from `/rev`. After load, `SelectRef` applies it: [CV:165-205, 240-243, 269-270]
  1. The name is resolved with `git rev-parse --symbolic-full-name --verify` (when that fails, the name is used as given). [GIT:895-910]
  2. A leading `refs/` is dropped.
  3. `heads/X` selects the **Branch** radio and branch `X`.
  4. `remotes/…` selects the **Branch** radio and that `remotes/…` entry.
  5. `tags/X` (with any `^{}` suffix removed) selects the **Tag** radio and tag `X`.
  6. Anything else (a hash, say) selects **Commit** and adds the string to the commit combo.

  `FindStringExact` returning -1 leaves the combo with no selection.
- **Branch "..."** opens Browse References as a picker (`CBrowseRefsDlg::PickRef(false, current, gPickRef_All)`). The picked full ref, or the old value on cancel, becomes the pending ref, and the lists are **reloaded** (`InitChooseVersion(true, true)`). [CV:148-163]
- **Commit "..."** opens the Log dialog in select mode:
  - single selection, no working-tree row, starting at the text already in the combo;
  - the selected hash is written into the commit combo;
  - it refuses with "This operation cannot be started while the log dialog is still loading commits." [POT:9557] if a log dialog thread is already running. [CV:94-117]
- **Resulting revision (`m_VersionName`)**, computed by `UpdateRevsionName` on OK: [CV:119-141]
  - HEAD gives `HEAD`.
  - Branch gives the combo text. If both `refs/heads/X` and `refs/tags/X` exist (`IsBranchTagNameUnique` false), it is prefixed with `refs/heads/`. [GIT:1575-1613]
  - Tag gives the combo text, prefixed with `refs/tags/` in the same ambiguous case.
  - Commit gives the combo text as typed.
  - `m_bIsBranch` is true only for the Branch radio. [CV:70-79]
- **Initial focus.** When `InitChooseVersion(true)` was used (Switch, Merge), the first enabled combo of branch, tag, version gets focus. [CV:245-254]
- **Anchors.** The combos and the group stretch horizontally, and the two "..." buttons stay at the top right. [CV:327-337]

### 8.2 Create Branch / Create Tag (`CCreateBranchTagDlg`, `IDD_NEW_BRANCH_TAG`)

#### 8.2.1 Window and layout

- **Template.** Caption "New Branch/Tag", 296×212 DLU, `WS_THICKFRAME`. [RC:1287-1289]
- **Class.** `CResizableStandAloneDialog`, so it resizes in both directions. Placement is saved as "BranchTagDlg". [CBTD:30-33, 149]
- **Title.** The title becomes `<wd> - Create Branch - TortoiseGit` or `… - Create Tag - …` [POT:3186, 3198], formatted as in 6.1. [CBTD:86-113]

Layout, top to bottom:

1. **"&Name" group.** [RC:1292-1294]
   - Label "Branch:", replaced at run time by "Branch" [POT:2137] or "Tag" [POT:9183] (no colon). [CBTD:90, 102]
   - Edit `IDC_BRANCH_TAG`, pre-filled from `/name` if given. [AU:1133-1134]
2. **"Base On" group** (8.1).
   - The HEAD radio text gets ` (<current branch>)` appended, for example `HEAD (master)`. [CBTD:119-122]
   - **Default radio: HEAD.** [CBTD:80]
3. **"Options" group**, one row. [RC:1305-1310]
   - "Trac&k": tri-state, default **indeterminate**, initially disabled.
   - "&Force"
   - "&Switch to new branch": branch mode only; hidden in tag mode **and in bare repositories**. [CBTD:115-118]
   - "Si&gn" and "&Push": tag mode only; hidden for branches. They sit at the same position as Switch.
4. **"&Message" group** [RC:1311-1312] with a multi-line plain edit (`ES_WANTRETURN`).
   - In branch mode the group caption becomes "Description" [POT:3506]. [CBTD:105]
   - The group and the edit stretch both ways. The Name and Options groups stretch horizontally. [CBTD:136-145]
5. **Buttons.** OK, Cancel, Help, bottom right. [RC:1313-1315]

**Tooltips.** [CBTD:151-153]
- Force: "Force creation of branch/tag - even if one with same name already exists." [POT:4683]
- Sign: "GPG signs the tag, see manual for details" [POT:4808]
- Track: "If checked --track is passed to git, if unchecked --no-track is passed to git, else neither --track nor --no-track is passed to git (see help)." [POT:5394]

#### 8.2.2 Option behaviour and auto-fill

- **Track** is enabled only in branch mode, with the Branch radio on, when the selected branch starts with `remotes/`. In every other case it is disabled (tag mode, HEAD, Tag, Commit). [CBTD:212-245, 260-267] Disabling does not reset its state. `DDX_Check` still reads a disabled box, so a state the user set earlier is passed on (inferred from MFC behaviour, not tested).
- **Name auto-fill from a remote branch.** Selecting `remotes/<remote>/<path>` fills the name with `<path>`, the text after the first `/` that follows `remotes/` (`Find('/', 9)`). It does so only when the name is empty, or still equals the last segment of the previously selected remote branch, so it never overwrites a name the user typed. [CBTD:214-236, 244]
- **Track auto-uncheck.** While the Branch radio is on, editing the name so that the remote branch no longer ends with `/<name>` sets Track to **unchecked**, which passes `--no-track`. [CBTD:247-258]
- **Sign** is enabled only when `git config user.signingkey` is non-empty. [CBTD:91]
- **Remembered between runs.**
  - "Switch to new branch" is pre-checked when `HKCU\Software\TortoiseGit\NewBranchSwitchTo` is set (default FALSE). It is also checked when the caller passes `switchNewBranch=true`. [CBTD:37, 106-110; AU:1128]
  - "Push" is pre-checked from `…\PushTag` (default FALSE). [CBTD:38, 93-97]
  - Both are written back on a successful OK. [CBTD:203-206]
  - The name, base and message are not remembered.

#### 8.2.3 Validation on OK (in order) [CBTD:162-210]

1. **Sign without a message.** A message box: "You must enter a log message for the commit" [POT:10589].
2. **Invalid name.** The name is trimmed first.
   - It is invalid when it contains any of `"|<>`, or when `git_branch_name_is_valid` rejects it. [GIT:263-270]
   - Error balloon on the edit, title "Error" [POT:4225]: "Branch/Tag name must not be empty or is invalid." [POT:2162]
3. **Name exists** (same kind: `refs/heads/` for a branch, `refs/tags/` for a tag) and Force is off. A warning balloon, title "Warning" [POT:10354]:
   - branch equal to the current branch: "Cannot force updating current branch. Use reset command if you want to do so." [POT:2312];
   - otherwise "A branch with this name already exists." [POT:1441] or "A tag with this name already exists." [POT:1473], plus " Use a different name or use the "Force" option to overwrite it." [POT:10019].

   With Force on, the current-branch check is skipped, and git itself reports any error.
4. **Name exists as the other kind.** A Continue / A&bort box [POT:2936, 1481]. Abort returns to the dialog.
   - For a branch: "A tag with the same name as your entered branch name already exists (so the short ref is ambiguous).\nThis can cause problems so you should avoid it." [POT:1469]
   - For a tag: the mirror text, "A branch with the same name as your entered tag name already exists…" [POT:1437]

#### 8.2.4 Commands run (`CAppUtils::CreateBranchTag`) [AU:1124-1211]

The command runs synchronously through `g_Git.Run`, with **no progress dialog**. On a non-zero exit, git's output is shown in an error message box. A temp file that cannot be written gives "Could not save tag message".

- **Tag:** `git.exe tag[ -f][ -s][ -F "<tmpfile>"] -- "<name>" "<base>"`
  - `-F` is used only when the trimmed message is non-empty, so a message makes an **annotated** tag and no message a **lightweight** one.
  - The message is saved with `SaveCommitUnicodeFile`.
  - `<base>` is `FixBranchName(m_VersionName)`, which is `HEAD` for the HEAD radio.
- **Branch:** `git.exe branch[ -f][ --track| --no-track] -- "<name>" "<base>"`
  - Track checked gives `--track`, unchecked gives `--no-track`, indeterminate passes neither (git's `branch.autoSetupMerge` default then applies).

Follow-ups, in this order:

1. If branch mode and Switch is checked: `PerformSwitch(name)` (8.3.4) with default options, so it runs `git.exe checkout --end-of-options "<name>" --` in the progress dialog.
2. If branch mode and the message is non-empty: `UpdateBranchDescription`.
   - It removes `\r` and trims.
   - It writes `branch.<name>.description`, or **unsets** that key when the text is now empty. [AU:1107-1122]
   - The description is written *after* the switch.
3. If tag mode and Push is checked: the Push dialog (7.3), called with `Push(hWnd, "refs/tags/<name>")`.

### 8.3 Switch/Checkout (`CGitSwitchDlg`, `IDD_GITSWITCH`)

#### 8.3.1 Window and layout

- **Template.** Caption "Switch/Checkout", 296×185 DLU. [RC:1259-1261]
- **Class.** `CHorizontalResizableStandAloneDialog`, so it resizes **horizontally only**. Placement is saved as "SwitchDlg". [GSWD:31-34, 102]
- **Title.** `<wd> - Switch/Checkout - TortoiseGit`. [GSWD:104]

Layout:

1. **"Switch To" group** (8.1, no HEAD radio). **Default radio: Branch**, which selects the current branch. [GSWD:106-108]
2. **"Option" group.** [RC:1273-1281]
   - row 1: "Create &New Branch" checkbox, with an edit `IDC_EDIT_BRANCH` (stretches) to its right
   - row 2: "Overwrite working tree changes (&force)"; on the right, "&Merge"
   - row 3: "T&rack" (tri-state)
   - row 4: "&Override branch if exists"
3. **Buttons.** OK, Cancel, Help, bottom right. [RC:1282-1284]

Initial focus is on "Create New Branch". [GSWD:109]

**Tooltips.** [GSWD:113-115]
- Override: the Force text [POT:4683].
- Merge: "Merge between original branch, working tree contents, and the branch to switch to" [POT:6140].
- Track: the Track text [POT:5394].

#### 8.3.2 Auto-fill (`SetDefaultName`)

`SetDefaultName` runs on every radio click, branch or tag selection change, commit-combo edit, and after the refs load. [GSWD:124-128, 173-223, 231-234, 249-252] It recomputes `m_VersionName` (8.1), then:

- **Remote branch.**
  - Applies when the name starts with `remotes/`; the new name is the text after the `/` that follows the remote name.
  - The code also has an `origin/` branch: it searches for a `/` from index 8, so `origin/x` does not match and `origin/a/b` gives `b`. This looks like a quirk.
  - Effect: the new branch name is set to the remote branch's path, "Create New Branch" is checked, and Track is enabled and set to **indeterminate**. [GSWD:181-196]
- **Otherwise.**
  - The new name becomes `Branch_<x>`, where `<x>` is:
    - the local branch name (a `refs/heads/` prefix is stripped);
    - or the tag name as resolved, which can be `refs/tags/…` when ambiguous;
    - or, for Commit, the first `GetShortHASHLength()` characters.
  - "Create New Branch" is **unchecked**.
  - Track is disabled and set to **unchecked**, so `--no-track` is passed if the user then checks "Create New Branch". [GSWD:197-209]
- **Remembered "new branch" defaults.** For Commit and Tag, "Create New Branch" is forced on when `HKCU\Software\TortoiseGit\SwitchToCommitNewBranch` or `…\SwitchToTagNewBranch` is set (both default TRUE). [GSWD:41-42, 211-212] On OK, the box's state is written back to the key of the selected radio. [GSWD:164-167]
- **Enabling.**
  - The edit and "Override branch if exists" are enabled only while "Create New Branch" is checked.
  - When "Create New Branch" is unchecked, Override and Track are cleared and Track is disabled. [GSWD:214-222]
  - Clicking the checkbox itself only toggles the edit. [GSWD:253-257]
- **Track auto-uncheck** on name edit works as in 8.2.2. [GSWD:236-247]
- Nothing else is remembered. Force and Merge default to unchecked. [GSWD:36-40]

#### 8.3.3 Validation on OK (only when "Create New Branch" is checked) [GSWD:135-172]

1. **Invalid name** (after trimming): error balloon "Branch/Tag name must not be empty or is invalid." [POT:2162]
2. **Branch exists** and Override is off: warning balloon "A branch with this name already exists." [POT:1441] + " Use a different name or use the "Override branch" option." [POT:10023]
3. **Tag with the same name exists:** the Continue/Abort box [POT:1469].

There is no check that a branch is actually selected. What happens with an empty selection was not established.

#### 8.3.4 Command (`CAppUtils::Switch` → `PerformSwitch`) [AU:1266-1378]

- `refs/heads/` is stripped from the target unless Override is checked. Without stripping, checkout would detach HEAD; git prefers branches over tags on name clashes. [AU:1278-1281]
- The new-branch name is passed only when "Create New Branch" is checked.
- The command line, run in `CProgressDlg`:

  `git.exe checkout[ -f][ --track| --no-track][ -B "<new>"| -b "<new>"][ --merge] --end-of-options "<ref>" --`

  - Track and `-b`/`-B` appear only with a new branch.
  - Track checked gives `--track`, unchecked gives `--no-track`, indeterminate gives neither.
  - `-B` is used when Override is checked. [AU:1293-1310]

**Post-exec rule.** With `--merge`, a zero exit while conflicts remain is turned into exit code 1 with the extra message "Has merge conflict". [AU:1368-1375]

**Post-command buttons.** [AU:1320-1367]
- On success:
  - "Update Submodules" [POT:9979], if the repository has submodules (`/command:subupdate /bkpath:<wd>`).
  - "&Merge..." [POT:842], if HEAD was on a branch before the switch. It opens the Merge dialog preselecting **that previous branch**.
  - "&Pull..." [POT:994], if HEAD is now on a branch.
  - "&Commit..." [POT:431].
- On failure:
  - "Resolve" [POT:7771], only with `--merge` and conflicts. Note that it runs `/command:commit`.
  - "Stash changes" [POT:8924], without `--merge`.
  - "&Retry" [POT:1082].
  - "Checkout with merge" [POT:2460], without `--merge`. It re-runs the same command with `--merge`.

### 8.4 Merge (`CMergeDlg`, `IDD_MERGE`)

#### 8.4.1 Pre-checks (`CAppUtils::Merge`) [AU:3378-3394]

- **User data.** `CheckUserData` loops while the user or committer name/email is empty: "User name and email must be set before commit.\nDo you want to set these now?" [POT:10213]. Yes opens Settings on `gitconfig`; No cancels. [AU:3039-3060]
- **Rebase lock.** The `tgitrebase.active` lock check (7.0, [POT:4724]). [AU:3771-3786]

#### 8.4.2 Window and layout

- **Template.** Caption "Merge", 328×274 DLU. [RC:1370-1372]
- **Class.** `CResizableStandAloneDialog`, both directions. Placement is saved as "MergeDlg". [MGD:32-35, 148]
- **Title.** `<wd> - Merge - TortoiseGit`. [MGD:118]

Layout:

1. **"Current branch:"** label with a read-only borderless edit `IDC_CURRENTBRANCH`. [RC:1375-1376]
2. **"From" group** (8.1, no HEAD radio).
   - **Default radio: Branch.** [MGD:115-116]
   - The branch list **omits the current branch** (`m_bSkipCurrentBranch`). [MGD:47]
3. **"Option" group.** [RC:1386-1396]
   - row 1: "&Squash"; "Messages" checkbox with a number edit (disabled until Messages is checked)
   - row 2: "No &Fast Forward"; "Fast Forward O&nly"
   - row 3: "No Co&mmit"
   - row 4: "Strategy" label, strategy drop-list, strategy-option drop-list (disabled), parameter edit (disabled)
4. **"Merge &Message" group** [RC:1397-1398] with a Scintilla editor (`CSciEdit`, initialised with the project properties and the log font). The group and editor stretch both ways. [MGD:105-107, 120-124]
5. **Buttons.** OK, Cancel, Help. [RC:1399-1401] Initial focus: OK. [MGD:149]

#### 8.4.3 Defaults and option interplay

- **Default "From".** With no initial ref, the dialog preselects the current branch's **push** target, `remotes/<pushremote>/<pushbranch>`. [MGD:151-161]
  - The push remote is `branch.<b>.pushremote`, else `remote.pushdefault`, else `branch.<b>.remote`.
  - The push branch is `branch.<b>.pushbranch` (not stripped), else `branch.<b>.merge` with `refs/heads/` stripped. [GIT:867-893]
  - If that entry is not in the list, the branch combo has no selection.
- **Message editor.**
  - It starts with "<Auto Generated by Git>" [POT:1409]. On OK that text counts as **empty**. [MGD:126-128, 185-189]
  - Its context menu adds "Paste &last commit message" and "Paste r&ecent message..." [POT:6947, 6951] when history exists. [MGD:226-271]
  - History: `HKCU\Software\TortoiseGit\History\merge\logmsgs*`, at most `MaxHistoryItems` entries (default 25). A non-empty, non-default message is saved on **OK and on Cancel**, unless No Commit is checked. [MGD:85-88, 130-131, 191-196, 206-217]
- **Messages (`--log`)** count: `git config merge.log` when it parses to an integer > 0, else **20**. [MGD:44-46] Checking Messages enables the count edit. [MGD:280-284]
- **Mutual exclusion.** [MGD:273-278, 299-306]
  - Squash disables the message editor and No Fast Forward.
  - No Fast Forward disables Fast Forward Only and Squash.
  - Fast Forward Only disables No Fast Forward.
- **Strategy.** [MGD:133-146, 286-297]
  - Strategies: `resolve`, `recursive`, `ours`, `subtree`. There is no empty entry; the initial state is no selection.
  - Options: `ours`, `theirs`, `patience`, `ignore-space-change`, `ignore-all-space`, `ignore-space-at-eol`, `renormalize`, `no-renormalize`, `rename-threshold`, `subtree`.
  - The option list is enabled only for `recursive`. The parameter edit is enabled only for `rename-threshold` and `subtree`.
  - On OK, an option is dropped unless the strategy is `recursive`, and a parameter is dropped unless the option takes one. [MGD:198-201]
- **Remembered between runs.** Only the message history. All checkboxes start unchecked. [MGD:38-42]
- **Validation.** If the Branch radio is on with no branch selected, a message box says "Branch/Tag name must not be empty or is invalid." [POT:2162]. [MGD:179-183]

#### 8.4.4 Command (`DoMerge`) [AU:3231-3376]

`git.exe merge[ --no-ff| --ff-only][ --squash][ --no-commit][ --allow-unrelated-histories][ --log=<N>][ --strategy=<s>[ --strategy-option=<o>[=<p>]]][ -F "<tmpfile>"] -- "<rev>"`

- `<rev>` is `FixBranchName(m_VersionName)`.
- `-F` is used only for a non-empty message ("Could not save merge message" on a temp-file failure).
- The dialog never sets `--allow-unrelated-histories` itself; only the post-command button below adds it.
- The command runs in `CProgressDlg`.

**On failure:**
- If conflicts exist:
  - An information box: "While merging, … a conflict in at least one file occurred. … After resolving all files, you need to perform a commit in order to complete the merge. If you want to abort the merge, do a hard reset on HEAD or select abort merge on the context menu. …" [POT:10419]. It has a "do not show again" check stored as `MergeConflictsNeedsCommit`.
  - Buttons "Resolve" (`/command:resolve /path:<wd>`) and "&Commit..." (`/command:commit`).
  - If the conflict check itself fails: "Checking for conflicts failed."
- If HEAD and `<rev>` have no merge base: "&Merge unrelated history" [POT:838] re-runs the same merge with `--allow-unrelated-histories`.
- Always: "Stash changes", which calls `StashSave` with `<rev>` passed along. What StashSave does with it was not traced.

**On success:**
- "Stash Pop" [POT:8916], if the caller asked for it (Pull's follow-up).
- With No Commit or Squash: only "&Commit...", and nothing else.
- Otherwise:
  - If the Branch radio was used and the name does not start with `remotes/`: "Remove &branch" [POT:7619]. It confirms with `Do you really want to delete "%s"?` [POT:3743] (&Delete / A&bort), then runs `git.exe branch -D -- "<rev>"` synchronously.
  - If the Branch radio was used: "Pus&h..." [POT:7277].
  - For git-svn repositories: "SVN DCommit..." [POT:8010].

**Closing.** If the progress dialog is closed with Cancel (Abort) and conflicts exist (`HasWorkingTreeConflicts() == 1`), the **Abort Merge dialog** opens immediately. [AU:3370-3374]

### 8.5 Abort Merge (`CMergeAbortDlg`, `IDD_MERGEABORT`)

#### 8.5.1 Window and layout

- **Template.** Caption "Abort Merge", 299×124 DLU, `DS_MODALFRAME`, **not resizable**. [RC:2020-2022]
- **Class.** `CStateStandAloneDialog`. Placement is saved as "MergeAbortDlg". [MAD:31-34, 66]
- **Title.** `<wd> - Abort Merge - TortoiseGit`. [MAD:60]

Layout:

1. Text: "In order to abort a merge progress a reset (to HEAD) is needed." [RC:2036]
2. **"Reset Type" group** with three radios: [RC:2025-2031]
   - "M&erge: Resets the index and try to reconstruct the pre-merge state"
   - "&Mixed: Leave working tree untouched, reset index"
   - "&Hard: Reset working tree and index (discard all local changes)"
3. **"Show modified files in &working tree"** button, full width. It opens `CFileDiffDlg` comparing HEAD with the working tree. [RC:2032; MAD:81-86]
4. **Buttons.** OK, Cancel, Help. [RC:2033-2035]

**Default: Merge**, every time; the choice is not remembered (`m_ResetType` starts at 0). [MAD:35, 69, 74-79] There is no further confirmation: OK runs the reset, even for Hard.

#### 8.5.2 Command (`CAppUtils::MergeAbort` → `Reset("HEAD", type)`) [AU:3396-3403, 1544-1653]

It does **not** use `git merge --abort`. The radio index maps Merge to reset type 3, Mixed to 1 and Hard to 2.

| Choice | Command | Notes |
|---|---|---|
| Merge | `git.exe reset --merge` | `CProgressDlg`. On failure the only button is "&Retry", which reopens the Abort Merge dialog. [AU:1558-1582] |
| Mixed | `git.exe reset --mixed --end-of-options "HEAD" --` | `CProgressDlg`, or the libgit2 `ResetProgressCommand` in `CGitProgressDlg` (6.4) when `UsingLibGit2(GIT_CMD_RESET)`. [AU:1590-1593, 1638-1650] |
| Hard | `git.exe reset --hard --end-of-options "HEAD" --` | Same runner as Mixed. |

Post-command buttons for Mixed and Hard: [AU:1601-1636]
- On failure: "&Retry", which re-runs the same reset.
- On success:
  - "Update Submodules" (Hard only, and only if the repository has submodules);
  - the four Bisect items, if a bisect is active;
  - "&Clean up..." [POT:403] (Hard only).

The Log refreshes after a successful Abort Merge. [GLLA:181-185]

### 8.6 Invocation from the Log context menu

The menu items and their visibility conditions are listed in 1.11. This section covers what each item passes. Ref labels are full ref names (`refs/heads/…`, `refs/remotes/…`, `refs/tags/…`).

**Clicked label.** Several items look at whether the right-click landed on a ref label of the row (`IsMouseOnRefLabelFromPopupMenu`, using `m_RefLabelPosMap`). If it did, a pointer to that label's ref is stored as the item's menu data. [GLLB:3751-3787] An item can require a ref type; then a label of another type does not count.

**Fallback.** When there is no label data, the handler uses either the row's first ref that starts with a given prefix (`GetFirstEntryStartingWith`) [GLLA:41-48] or the commit hash.

| Log item | Label used (type filter) | Fallback | Calls | Effect in the dialog |
|---|---|---|---|---|
| "Create Br&anch at this version..." | clicked label, **remote branches only** [GLLB:2038-2046] | first `refs/remotes/` ref of the row, else the hash [GLLA:531-536] | `CreateBranchTag(isTag=false, &ref)` [GLLA:538] | remote: Branch radio, name auto-filled, Track enabled; hash: Commit radio |
| "Create &Tag at this version..." | none (no menu data is set) [GLLB:2048-2049] | same: first `refs/remotes/` ref, else the hash | `CreateBranchTag(isTag=true, &ref)` | Branch radio on the remote branch, or Commit radio |
| "Sw&itch/Checkout to this..." | clicked label, any type [GLLB:2029-2036] | first `refs/remotes/` ref, else the hash [GLLA:546-555] | `Switch(ref)` | remote: new branch with track (8.3.2); hash: Commit radio with `Branch_<short>` |
| `&Switch/Checkout to "name"` / submenu | the clicked local branch, or all local branches of the row except the current one [GLLB:1982-2027] | none | `PerformSwitch(shortname)`: **no dialog**, plain `git.exe checkout --end-of-options "<name>" --` [GLLA:561-575] | none |
| `&Merge to "<current>"...` | clicked label, any type [GLLB:1964-1974] | the row's **first ref of any kind**, else the hash [GLLA:1104-1117] | `Merge(&ref)` | radio chosen by 8.1 `SelectRef`; overrides the push-branch default |
| "Abort &Merge" (working-tree row, merge in progress) | none | none | `MergeAbort()` [GLLA:181-185; GLLB:1760-1763] | none |

After Create Branch/Tag and Switch, the Log reloads its ref map and repaints, then posts `MSG_REFLOG_CHANGED`. After Merge it calls `Refresh()`. [GLLA:539-543, 557-559, 1113-1116]

**Shared implementation.** All entry points (Explorer via `/rev`, the Log, Browse References, the Revision Graph) reduce to one input: an optional **initial ref string**, which is a full ref, a short ref or a hash. The dialog's `SelectRef` turns it into the radio and combo state. Only the Log's direct "Switch/Checkout to <branch>" bypasses the dialog.

### 8.7 Not established from source

- Whether `CBS_SORT` on the ComboBoxEx controls changes the order produced by `GetBranchList`/`GetTagList`.
- Whether the libgit2 branch listing includes `<remote>/HEAD` (the CLI path skips it).
- What happens in Switch when the Branch radio is on but no branch is selected, and in Create Branch when a disabled Track box keeps a checked state.
- What `StashSave(..., mergeVersion)` does after stashing (the Merge failure button).
- The Help topics of the four dialogs.

## 9. Conflicts: Edit conflicts and merge tools

"Edit conflicts" is a single routine, `CAppUtils::ConflictEdit`. It reads the index stages of one conflicted path, chooses one of three UIs, and starts it:

- the configured three-way merge tool, for a normal content conflict;
- the "Conflict" dialog, for a delete/modify conflict;
- the "Resolve Submodule Conflict" dialog, for any conflict that involves a gitlink.

Marking a file as resolved is a separate step. Usually it is TortoiseGitMerge's "Mark as resolved", which calls the Resolve command (6.5.1), or the list's "Resolved" item. TortoiseGit never uses git's `mergetool` machinery. This section covers the routine, the Merge Tool settings, and where the command is offered. The Merge dialog and Abort Merge are in section 8.

New citation keys (same commit as section 0):

| Key | File |
|---|---|
| **AU.h** | `src/TortoiseProc/AppUtils.h` |
| **CEC** | `src/TortoiseProc/Commands/ConflictEditorCommand.cpp` |
| **DCD** | `src/TortoiseProc/DeleteConflictDlg.cpp` (`IDD_RESOLVE_CONFLICT`) |
| **SRCD** | `src/TortoiseProc/SubmoduleResolveConflictDlg.cpp` |
| **SPM** / **SPM.h** | `src/TortoiseProc/Settings/SettingsProgsMerge.cpp` / `.h` |
| **SPAD** | `src/TortoiseProc/Settings/SetProgsAdvDlg.cpp` |
| **TAD** | `src/TortoiseProc/Settings/ToolAssocDlg.cpp` |
| **SET** | `src/TortoiseProc/Settings/Settings.cpp` |
| **SADV** | `src/TortoiseProc/Settings/SettingsAdvanced.cpp` |
| **STRU** | `src/Utils/StringUtils.cpp` |
| **CLP** | `src/Utils/CmdLineParser.cpp` |
| **TMA** | `src/TortoiseMerge/TortoiseMerge.cpp` (command-line parsing, exit code) |
| **TMDD** | `src/TortoiseMerge/DiffData.cpp` |
| **FTL** | `src/TortoiseMerge/FileTextLines.cpp` |
| **TMRC** | `src/Resources/TortoiseMergeENG.rc` (TortoiseGitMerge strings; these are not in POT) |
| **SSDP** | `src/TortoiseProc/Settings/SetSavedDataPage.cpp` |
| **TIDM** | `src/TortoiseIDiff/MainWindow.cpp` |
| **DSCR** | `contrib/diff-scripts/` (installed as `Diff-Scripts` next to `bin`) |
| **MAN-CONF** | `doc/source/en/TortoiseGit/tgit_dug/dug_conflicts.xml` |
| **MAN-PROGS** | `doc/source/en/TortoiseGit/tgit_dug/dug_settings_progs.xml` |
| **MAN-ADV** | `doc/source/en/TortoiseGit/tgit_dug/dug_settings_advanced.xml` |

### 9.1 Edit conflicts (`CAppUtils::ConflictEdit`)

The signature is `ConflictEdit(HWND, CTGitPath& file, bool bAlternativeTool = false, bool isRebase = false, HWND resolveMsgHwnd = nullptr)`. [AU.h:172; AU:1755]

#### 9.1.1 Reading the stages

1. The routine runs `git.exe ls-files -u -t -z -- <path>`. It returns FALSE silently if git fails. [AU:1773-1775]
2. `CGit::ParseConflictHashesFromLsFile` reads the stage-1, 2 and 3 hashes into `baseHash`, `localHash` and `remoteHash`. A stage with no entry keeps an empty hash. Each stage is also flagged "is file", which means mode ≠ `160000`. Symlinks (`120000`) therefore count as files. Empty or malformed output makes the routine return FALSE. [GIT:3948-4013; AU:1781-1784]
3. The titles come from `GetConflictTitles` (9.1.6), computed before anything else. [AU:1777-1779]

#### 9.1.2 Choosing the UI

| Stage situation | UI |
|---|---|
| Any stage is a gitlink (`160000`) | "Resolve Submodule Conflict" dialog (9.1.5) [AU:1786-1893] |
| Stage 2 **and** stage 3 both present (both modified; also **added by both**, where stage 1 is absent) | Merge tool (9.1.3) [AU:1943-1950] |
| Stage 2 or stage 3 absent (deleted on one side, or on both) | "Conflict" (delete/modify) dialog (9.1.4) [AU:1951-2006] |

#### 9.1.3 Temp files and launching the tool

**Naming.** `GetMergeTempFile(type, path)` builds `<path as written>.<TYPE><ext>`. `<ext>` is the extension *including the dot*, or empty when there is none. The result is made absolute against the working-tree root. [AU:1689-1695; TGP:490-501]

- `src/a.txt` gives `src/a.txt.BASE.txt`, `src/a.txt.LOCAL.txt` and `src/a.txt.REMOTE.txt`.
- `Makefile` gives `Makefile.BASE`, and so on.

The files sit **next to the conflicted file in the working tree**. The manual's `filename.ext.BASE.ext` matches this [MAN-CONF:40-69]. The settings page's wording `CONFLICTED.BASE.EXT` [MAN-PROGS:294] is looser.

**Which stage goes where** [AU:1900-1941]:

| | Not a rebase | Rebase (`isRebase`) |
|---|---|---|
| `mine` variable | `.LOCAL` file | `.REMOTE` file |
| `theirs` variable | `.REMOTE` file | `.LOCAL` file |
| stage 1 is written to | `.BASE` | `.BASE` |
| stage 2 is written to | `mine` = `.LOCAL` | `mine` = `.REMOTE` |
| stage 3 is written to | `theirs` = `.REMOTE` | `theirs` = `.LOCAL` |

During a rebase, `.LOCAL` therefore holds stage 3 (the commit being replayed) and `.REMOTE` holds stage 2 (the onto side). That matches the manual: "mine" is the branch being rebased. [MAN-CONF:57, 69]

**Extraction steps:**

1. All three files are first created **empty** ("in case stage is not three"). [AU:1912-1919]
2. Each present stage N runs `git.exe checkout-index --temp --stage=N -- <path>`. The temp name is the text before the first tab in git's output, and it is moved over the target with `MoveFileEx(REPLACE_EXISTING | COPY_ALLOWED)`. A git error shows `output + "\n" + err` in a message box, and launching continues anyway. [AU:1921-1941]
3. A **missing base** (added by both) therefore leaves an empty `.BASE` file, and the tool does a two-way-plus-empty-base merge. [AU:1913-1941]
4. There is no special handling of **binary** content or **symlinks** in this routine. For what TortoiseGitMerge does with binaries, see 9.1.3a. What `checkout-index --temp` writes for a symlink stage (a link, or a text file holding the target) was not verified.

**Launch** [AU:1943-1950]. The merged path is made absolute, and then:

- Not a rebase: `StartExtMerge(alt, base, theirs=.REMOTE, mine=.LOCAL, merged, "Base", theirsTitle, mineTitle, mergedname="", readonly=false, resolveMsgHwnd, deleteBaseTheirsMineOnClose=true)`
- Rebase: `StartExtMerge(alt, base, theirs=<stage-2 file>, mine=<stage-3 file>, merged, "Base", mineTitle, theirsTitle, …)`

So the tool's "theirs" pane always shows stage 2 during a rebase, labelled with the onto name, and "mine" shows the replayed commit, labelled with the branch name.

**Picking the command (`StartExtMerge`)** [AU:264-400]:

1. `com` is read from the following, each later step overriding the earlier ones when non-empty [AU:280-295]:
   - `HKCU\Software\TortoiseGit\Merge`
   - `MergeTools\<.ext lowercased>` (the extension-specific value)
   - `MergeTools\.<filename lowercased>` (a whole-filename value, e.g. `MergeTools\.makefile`)
2. **Alternative** (Shift, or `/alternative`) with a non-empty `com` [AU:297-303]:
   - A leading `#` is stripped. `#` marks a "disabled" external command (9.2), so Shift then **uses** it.
   - Otherwise `com` is emptied, so Shift falls back to the **built-in** tool.
3. If `com` is empty or still starts with `#`, the built-in tool is used [AU:305-346]:
   - For `.jpg .jpeg .bmp .gif .png .ico .tif .tiff .dib .emf .cur .webp .svg .svgz` the tool is **TortoiseGitIDiff**:
     `"<bin>\TortoiseGitIDiff.exe" /base:%base /theirs:%theirs /mine:%mine /result:%merged /basetitle:%bname /theirstitle:%tname /minetitle:%yname [/resolvemsghwnd:<n>] [/deletebasetheirsmineonclose]`
   - Otherwise the default tool is **TortoiseGitMerge**:
     `"<bin>\TortoiseGitMerge.exe" /base:%base /theirs:%theirs /mine:%mine /merged:%merged /basename:%bname /theirsname:%tname /minename:%yname /mergedname:%mname /saverequired [/resolvemsghwnd:<n>] [/deletebasetheirsmineonclose]`
   - Both get ` /groupuuid:"<uuid>"` when a grouping UUID is set.
   - `ConflictEdit` always passes `deleteBaseTheirsMineOnClose=true`. `/resolvemsghwnd` is passed only when the caller has a window to refresh. The Explorer command has none [CEC:24-29].
4. An external command that contains none of `%base`, `%theirs`, `%mine` or `%merged` gets ` %base %theirs %mine %merged` appended. [AU:347-349]
5. **Placeholders** are `%base %theirs %mine %merged %bname %tname %yname %mname %wtroot` [AU:351-362]:
   - An empty title falls back to the file's name (`GetUIFileOrDirectoryName`). `%mname` is always the merged file's name here, and `%wtroot` is the working-tree root. [TGP:484-488]
   - Every value is quoted. If the template already has `"` immediately around a placeholder, those quotes are dropped, so nothing is quoted twice. [STRU:821-863]
   - For the built-in tools a `"` inside a value is doubled (`CCmdLineParser::EscapeValue`). For external tools it becomes `%22` and trailing backslashes are trimmed. [CLP:243-264; AU:255-261]
   - Example of the default for a plain merge (inferred from the code above; the path is illustrative): `"…\TortoiseGitMerge.exe" /base:"C:\r\a.txt.BASE.txt" /theirs:"C:\r\a.txt.REMOTE.txt" /mine:"C:\r\a.txt.LOCAL.txt" /merged:"C:\r\a.txt" /basename:"Base" /theirsname:"MERGE_HEAD (feature, 1a2b3c4)" /minename:"HEAD" /mergedname:"a.txt" /saverequired /resolvemsghwnd:… /deletebasetheirsmineonclose`
6. **Launch.** The process is started with `IDS_ERR_EXTMERGESTART` "Could not start external merge program!\n\n%s" [POT:3119] as its failure message. [AU:364-370]
7. **Waiting.** Only for an **external** tool, and only if `MergeBlockTrustBehavior` ≥ 1 (default 0). The built-in tools are **never** waited for. [AU:367-373]

**After a blocking external tool exits** [AU:375-398]:

1. The three temp files are **always** deleted.
2. Then:
   - `MergeBlockTrustBehavior = 2` and exit code 0: `git.exe add -f -- <path>` runs silently.
   - `MergeBlockTrustBehavior = 2` and any other exit code: nothing happens and there is no prompt. The file stays conflicted.
   - `MergeBlockTrustBehavior = 1`: a question box "Was the merge of\n%s\nsuccessful?" [POT:10362] with buttons "&Resolved" [POT:1070] / "&No" [POT:894]. Resolved runs `git add -f`.
   - A failing `add` shows git's output in a message box.
3. After a successful add, `GITSLNM_NEEDSREFRESH` is posted to `resolveMsgHwnd` if `RefreshFileListAfterResolvingConflict` is TRUE (the default).

**Non-blocking external tools** return immediately. The temp files remain until the conflict is marked resolved through TortoiseGit; the Resolve command deletes them (6.5.4). [AU:371-373; MAN-PROGS:294-299]

**Conflict markers are never checked by TortoiseGit itself.** No code in TortoiseProc scans a resolved file for `<<<<<<<`. The only guards are the index-based ones:

- `HasWorkingTreeConflicts`, used by the Rebase dialog (9.3.3);
- TortoiseGitMerge's own view state (9.1.3a);
- the commit-message "# Conflicts:" hint check (9.3.3).

This was established by searching the source for the marker strings: the only hits are TortoiseGitMerge's own writer.

#### 9.1.3a What TortoiseGitMerge does with these arguments (brief)

- **Flags.** `/saverequired` sets `m_bSaveRequired`. `/deletebasetheirsmineonclose` and `/resolvemsghwnd` are stored. [TMA:195-200, 383]
- **Initial content.** The bottom (merged) view is computed by a three-way diff of base, theirs and mine. `Load()` reads only those three files, so whatever text the working-tree file has, conflict markers included, is not the starting content. [TMDD:183-240]
- **Save required.** After loading, "save required" is cleared again if the automatic merge left conflicts and `/saverequiredonconflicts` was not given. [TMF:1114-1116]
- **Binary files.** A file whose content has an aligned all-zero DWORD is treated as binary. Loading then fails with "The file\n%s\nis not a valid text file!" [FTL:70-77, 282-284; TMRC:933]
  - The manual's advice for binaries is to use Resolve with "mine" or "theirs" instead. [MAN-CONF:92-100]
- **Saving with conflicts left.** A task dialog appears: "Unresolved conflicts!" / "There are still unresolved conflicts in line %d! …". Its command links are "Save" ("Save the file with the conflict markers.") and "Cancel" ("Resolve the conflicts first."), and Cancel is the default. [TMF:3078-3103; TMRC:857-859, 1120-1124]
- **Saving with no conflicts left.** If the index still has stage 1 or stage 2 for the path (checked with libgit2), it asks "Do you want to mark the file\n%s\nas resolved?" ("File has no conflicts"). The command links are "Mark as resolved" (default) and "Leave as conflicted". [TMF:1737-1786; TMRC:1172-1176]
- **Mark as resolved** (the prompt above, or the menu item) runs `TortoiseGitProc /command:resolve /path:<merged> /closeonend:1 /noquestion /skipcheck /silent [/resolvemsghwnd:…]`. That stages the file, deletes the temp files and refreshes the caller (6.5.1). It runs once per session. [TMF:2244-2284]
- **Closing.** When the window is closed, or another file is opened (and the save prompt was not cancelled), the base, theirs and mine files are deleted because of `/deletebasetheirsmineonclose`. [TMF:2695-2799]
- **Exit code.** The process returns 1 if conflicts remain at close and 0 otherwise. TortoiseGit does not wait for it, so the exit code is unused here. [TMA:435-445; AU:368]
- **TortoiseGitIDiff** has its own resolve path: it asks with the `IDS_MARKASRESOLVED` text and runs the same `/command:resolve … /silent` line. [TIDM:800-815]

#### 9.1.4 Delete/modify conflicts ("Conflict" dialog, `IDD_RESOLVE_CONFLICT`)

This dialog is used when stage 2 or stage 3 is missing. [AU:1951-2006]

**Temp files.** `.LOCAL` and `.REMOTE` are deleted at once. `.BASE` is kept while the dialog is open, for "Show changes", and deleted on exit. Without a base it is deleted at once. [AU:1953-1961]

**Window** [RC:1533-1552]:

- 315×129 DLU, caption **"Conflict"**; the window title is the file's full path [DCD:98].
- Group box "Delete/modify merge conflict"; an info label shows the git path [DCD:100].
- Two rows. Each has:
  - a read-only ref box showing the side's title;
  - a status text: "Deleted" [POT:3454] if that side lacks the file, "Modified" [POT:6208] if it has the file and a base exists, otherwise "Created" [POT:3222]. [AU:1667-1680]
  - "Show log" (`/command:log /path:<file> /endrev:<hash> /rev:<hash>`; hidden when the ref text is empty) [DCD:87-96, 132-137];
  - "Show &changes", hidden and disabled by default.
- During a rebase the two rows are swapped, so the top row shows the theirs-title side. [AU:1964-1982]
- Buttons:
  - **"OK"**, relabelled "Modified" when a base exists and "Created" otherwise [DCD:71-86]. It keeps the file.
  - **"Delete"**
  - **"Abort"**, the default, which cancels
  - "Help"
- "Show changes" appears only when a base exists, on the side that still has the file. It diffs `.BASE` against the working file (Shift: alternative diff tool). [DCD:71-83, 139-151]

**Results** [AU:1983-2005]:

- Keep runs `git.exe add -- <path>` (no `-f`). The list entry becomes ADDED instead of UNMERGED.
- Delete runs `git.exe rm -- <path>`.
- A git failure shows its output in a message box.

#### 9.1.5 Submodule conflicts ("Resolve Submodule Conflict", `IDD_RESOLVESUBMODULECONFLICT`)

**Before the dialog** [AU:1786-1882]:

- If the path is an initialized submodule (a working-tree root), its `HEAD` replaces the base hash. The change type of each side (Identical, New, Delete, Fast-forward, Rewind, Newer/Older/Same time) and the commit subjects are computed inside the submodule.
- Otherwise the subjects are fixed strings: "not initialized" [POT:10963], "no submodule" [POT:10943] or "file, not a submodule" [POT:10824], depending on which stages exist.
- An unmatched combination returns FALSE.

**Window** [RC:2153-2184; SRCD:66-122]:

- 381×209 DLU, resizable horizontally; window title from the repository root.
- Header: `Submodule "<path>"`.
- Three groups titled with the Base, Mine and Theirs titles. Each shows Revision, Subject and "Show log"; Mine and Theirs also show Type and a **"Use this"** button.
- Invalid subjects get the "invalid" brush, and non-identical types get a change-type colour.
- During a rebase Mine and Theirs are swapped. [SRCD:160-195]

**"Use this"** [SRCD:223-253]:

1. Confirm with "Are you sure you want to mark the conflicted file(s) as resolved?" [POT:1838].
2. Run `ResolveProgressCommand` with Mine or Theirs (6.5.5) in an auto-closing progress dialog.
3. On success, the dialog closes. `ConflictEdit` then posts `GITSLNM_NEEDSREFRESH` to its caller. [AU:1884-1892]

The manual notes that an **uninitialized** submodule cannot be resolved automatically. [MAN-CONF:157-162]

#### 9.1.6 Conflict titles (`GetConflictTitles`)

- **Base** is always "Base" [POT:2033].
- **Rebase** (`rebaseActive`) [AU:1697-1726]:
  - mine = the contents of `.git/tgitrebase.active/onto`. If that file is missing, the default is "Branch being rebased onto", or the ref guessed for `rebase-apply/onto` plus `, <short hash>`.
  - theirs = `tgitrebase.active/head-name`. If that is missing, the default is "Branch being rebased", or `rebase-apply/head-name` with `refs/heads/` stripped.
  - These English strings are hard-coded.
- **Otherwise** mine = **"HEAD"**, and theirs comes from the first of these refs that exists [AU:1728-1752; POT:173, 6914, 10787]:

| Ref present | Theirs label | Situation |
|---|---|---|
| `MERGE_HEAD` | `MERGE_HEAD (<guessed ref>, <short hash>)`, or `MERGE_HEAD (<short hash>)` | merge, pull |
| `CHERRY_PICK_HEAD` | `CHERRY_PICK_HEAD (<short hash>)` (no ref guessing) | cherry-pick, including the Rebase dialog in cherry-pick mode |
| `REVERT_HEAD` | `Parent of <short hash>` | revert |
| none | "changes to-be-integrated" | **stash apply/pop**, and anything else |

- **Ref guessing** can be turned off with `ConflictDontGuessBranchNames` (Advanced settings, default false). The value is read once per process. [AU:1700; SADV:38; MAN-ADV:95-101]
- **"Rebase active"** means that `.git/rebase-apply` or `.git/tgitrebase.active` exists [TGP:857-866]:
  - The Explorer command [CEC:24-29] and the shared file list (`m_bIsRevertTheirMy`, set on every `GetStatus`) [GSLC:385-387] both use this test.
  - git's default `rebase-merge` backend directory is **not** checked. A conflict from a command-line `git rebase` (merge backend) is therefore treated as non-rebase: no swap, and theirs is "changes to-be-integrated" unless `CHERRY_PICK_HEAD` exists. This is inferred from the code and was not tested.

### 9.2 Merge tool settings (Settings > Diff Viewer > Merge Tool)

**The page.** `CSettingsProgsMerge` is a child page of the Diff Viewer page. [SET:137-138]

`IDD_SETTINGSPROGSMERGE` (300×217 DLU), caption **"Merge Tool"** [RC:683-698]:

| Control | Label | Behaviour |
|---|---|---|
| static | "Configure the program used to resolve conflicted files." | – |
| radio `IDC_EXTMERGE_OFF` | "&TortoiseGitMerge" | – |
| radio `IDC_EXTMERGE_ON` | "&External" | enables the edit box and "..." |
| edit `IDC_EXTMERGE` | command line | filesystem autocomplete; tooltip lists `%merged %theirs %mine %base` [RC:3728-3729] |
| "..." | file picker | – |
| check `IDC_MERGEBLOCK` | "Block TortoiseGit while executing the external merge tool" | enabled only with External |
| check `IDC_TRUSTEXITCODE` | "Trust the exitcode of the external merge tool for auto resolving" | enabled only with External and Block checked; unchecking Block also unchecks it |
| static and "&Advanced..." | "Click on "Advanced" to specify alternate merge programs based on file extension" | opens the per-extension dialog |

[SPM:42-55, 166-186]

**Registry storage** [SPM:27-35, 70-110, 156-164; SPM.h:57]:

- **`Merge`** (REG_SZ, default empty) holds the command:
  - The External radio is selected iff the value is non-empty and does not start with `#`.
  - Choosing TortoiseGitMerge with a non-empty command **keeps** the command and prefixes it with `#`. Choosing External strips the `#`.
  - The `#` command is what Shift ("alternative") launches (9.1.3).
- **`MergeBlockTrustBehavior`** (DWORD, default 0):
  - 0 means don't wait; 1 means Block; 2 means Block and trust the exit code.
  - Unchecking both writes 0 only if the value already exists.

**Advanced dialog** [RC:508-519, 534-544; SPAD:27-31, 275-282; TAD:36-47]:

- `IDD_SETTINGSPROGSADV`, titled "Advanced merge settings" [RC:3676]:
  - a group "Extension specific programs" holding a list with columns "Extension" / "Program" [RC:3565-3566];
  - buttons "&Add...", "&Edit...", "&Remove", "Re&store defaults", OK, Cancel.
- Entries are stored as values under `HKCU\Software\TortoiseGit\MergeTools`: the name is the extension (e.g. `.bmp`), the data is the command.
- Add and Edit open `IDD_TOOLASSOC`, titled "Add/Edit extension specific merge program" [RC:3672-3674]:
  - fields "Extension:" (lower-cased) and "External Program:" with "...";
  - a leading `*` is trimmed from the extension unless it contains `/`.
- A filename key such as `.makefile` also works, because of the lookup in 9.1.3.

**Shipped defaults (diff scripts)** [AU:971-1053; TP:456, 546, 587; DSCR]:

- `SetupDiffScripts` scans `<install>\Diff-Scripts\merge-*.js|vbs`. The extensions come from the file name suffix plus a first line `// extensions: a;b;c`.
- For each extension it writes `MergeTools\<.ext>` = `wscript.exe "<script>" %merged %theirs %mine %base` (+ ` //E:javascript` or ` //E:vbscript`).
- It runs only when the value is empty or already points at that script, on every version upgrade, and always (forced) on "Restore defaults".
- Shipped merge scripts:
  - `merge-doc.js` for `doc;docx;docm`
  - `merge-odt.js` for `ods;odt;sxw`

**git's `merge.tool` / `mergetool.*` config is not used.** The only "mergetool" identifiers in the source are TortoiseGit's own registry keys (search of `src/`).

**Documented placeholders.** The manual lists `%base %bname %mine %yname %theirs %tname %merged %mname %wtroot`. [MAN-PROGS:196-270]

**Documented example command lines** (exact text) [MAN-PROGS:276-291]:

```
C:\Path-To\P4Merge.exe %base %theirs %mine %merged
C:\Path-To\kdiff3.exe %base %mine %theirs -o %merged --L1 %bname --L2 %yname --L3 %tname
C:\Path-To\compare.exe /max /wait /3 /title1:%tname /title2:%bname /title3:%yname %theirs %base %mine %merged /a2
C:\Path-To\WinMergeU.exe /e /ub /fr /wl /wm /dl %bname /dm %tname /dr %yname  %base %theirs %mine /o %merged /ar
```

These are, in order, Perforce Merge, KDiff3, Araxis, and WinMerge 2.12 or later. The WinMerge line contains a double space in the original.

- No merge example is given for Beyond Compare, Meld or SemanticMerge.
- The Diff Viewer section gives only *diff* examples (ExamDiff, KDiff3, WinMerge, Araxis). [MAN-PROGS:118-130]

**Manual on temp files and blocking.** The temp files are removed when the conflict is marked resolved through TortoiseGit, TortoiseGitMerge or TortoiseGitIDiff. With an external tool the file must be marked resolved manually, unless Block or Trust is used. "The temporary files are removed in any case" when blocking. [MAN-PROGS:294-299]

### 9.3 Where Edit conflicts is offered

#### 9.3.1 Explorer and the command line

- **Explorer menu.** "&Edit conflicts" (`IDS_MENUCONFLICT` [POT:571]) appears for a conflicted non-folder (table 4.2 row 33). It runs `/command:conflicteditor /path:<file>`, adding `/alternative` when Shift is held. [CTX:1422-1423]
- **The command.** It calls `ConflictEdit(explorer hwnd, path, /alternative, IsRebaseActive())` with no refresh window. [CEC:24-29]
- **Neighbouring commands.** Explorer also has "Resolve..." (6.5); the manual describes it as the multi-file route. [MAN-CONF:101-110]

#### 9.3.2 The shared file list (`CGitStatusListCtrl`)

This list is used in Commit, Check for modifications, Resolve, Rebase and Sync.

- **Context menu.** For an UNMERGED selection the first group is:
  - "&Edit conflicts": single selection only, mask `GITSLC_POPCONFLICT`, and the **default item**;
  - "Resolved";
  - two `Resolve conflict using "%s"` items, titled from `GetConflictTitles` (5.2.8).

  [GSLC:1691-1719]
- **Running it.** Choosing it (Shift: alternative tool) calls `ConflictEdit(..., m_bIsRevertTheirMy, logical parent hwnd)`. When that returns TRUE:
  - if the file no longer exists, the parent refreshes;
  - otherwise the list re-shows, which picks up an entry the delete/modify dialog changed from UNMERGED to ADDED.

  [GSLC:2448-2461]
- **Double-click and Enter.** On a conflicted row these run the same `ConflictEdit` (Shift: alternative) instead of a diff, with the same refresh rules. [GSLC:2953-2965, 3639-3666]
- **Refresh after the tool.** For TortoiseGitMerge the list is refreshed later, when the Resolve command posts `GITSLNM_NEEDSREFRESH` to the parent (6.5.1). The dialogs that handle it are Add, Changed, Commit, Log, Rebase, Resolve and Revert. [GSLC:57-58; RD:84]

#### 9.3.3 Rebase dialog, conflicted step

This complements 2.7, steps 3 and 5.

**Entering the conflict state** [RD:2336-2345]:

- A cherry-pick that fails with conflicts sets the stage to `Conclict`, or `Squash_Conclict` for a squash row.
- The taskbar turns red.

**`ListConflictFile`** [RD:2449-2496]:

1. In rebase mode (not cherry-pick mode) it first creates `.git/tgitrebase.active/` with `head-name` (branch combo text) and `onto` (Onto, else upstream combo text). `IsRebaseActive()` is therefore true from here on, and Edit conflicts, Resolve-using and the submodule dialog all swap mine and theirs (9.1.6).
2. It loads the status and shows UNMERGED, MODIFIED, ADDED, DELETED and REPLACED rows, with only UNMERGED pre-checked, then checks all files.
3. If any row is a directory (a submodule), it adds ", take care of submodule changes" [POT:11029] in red to the status text.

The folder is removed by `CleanUpRebaseActiveFolder` [RD:106-117].

**The list.** Its context menu is `POPALL` minus Commit, Restore and Changelists [RD:243], so Edit conflicts, Resolved and Resolve-using are all available.

**Refreshing.** `GITSLNM_NEEDSREFRESH` and F5 call `Refresh()`, which in a conflict stage re-runs `ListConflictFile`. [RD:2733-2745]

**UI in the conflict state** [RD:2521-2545, 1810-1822]:

- The "Conflict Files" tab is activated and the message box becomes editable. In cherry-pick mode it is pre-filled from `MERGE_MSG`; otherwise, or if that is empty, it uses the commit's subject and body.
- The button reads **"Commit"** in `Conclict` and **"Continue"** in `Squash_Conclict`.

**What Commit/Continue requires** [RD:1096-1121, 1350-1376; AU:2181-2206]:

1. **No unmerged index entries** (`g_Git.HasWorkingTreeConflicts()`, not a marker scan):
   - Otherwise the box "One or more files are in a conflicted state." [POT:6690] appears, the selection is cleared, and the first UNMERGED row is selected, scrolled into view and focused.
   - If the check itself fails, "Checking for conflicts failed." is logged.
2. **No "# Conflicts:" hint in the message**:
   - The hint is the text `\n<commentchar> Conflicts:\n<commentchar>\t`.
   - The check is skipped when `StripCommentedLines` is on or when `core.cleanup` is verbatim, whitespace or scissors.
   - It shows `IDS_CONFLICT_HINT_IN_COMMIT_MESSAGE` [POT:5683] with buttons "&Ignore" / "A&bort" and a "do not show again" checkbox. Abort returns to editing.
3. **Then:**
   - `Conclict` stages the checked rows and unstages the unchecked ones, then runs `git commit --allow-empty-message -C <orig>` (plus `--amend -F` if the message was edited). If the row's action was Edit it moves to Edit; otherwise it continues (2.7 step 3).
   - `Squash_Conclict` does no staging. It moves to `Squash_Edit` if the next row is not a squash, else marks the row done and continues.

**Skip.** There is no Skip button. Changing the current row's action to **Skip** (context menu or the `S` key) while in `Conclict` or `Error` runs `git reset --hard` and continues (2.5). [RD:2955-2972]

**Abort.** As in 2.7 step 8 (confirmation, then restore). The manual tells users to commit rebase and cherry-pick conflicts from the Rebase or cherry-pick dialog, not from the Commit dialog. [MAN-CONF:81, 124]

#### 9.3.4 Resolve dialog and progress-dialog post-commands

- **Resolve dialog (6.5).** Its list keeps "&Edit conflicts" in the context menu, and double-clicking a row edits the conflict. OK marks every checked row as resolved (`git add -f`) without opening a tool. [RSD:60, 145]
- **"Resolve" post-command button** (`IDS_PROGRS_CMD_RESOLVE` "Resolve" [POT:7771]):
  - After a **pull** (`DoPull` [AU:2326-2350]) or **merge** (`DoMerge` [AU:3289-3312]) that leaves conflicts, it runs `/command:resolve /path:"<root>"`. A "&Commit..." button next to it runs `/command:commit`.
  - The same label in three other places runs **`/command:commit`**, not resolve (quirk):
    - switch-with-merge (`PerformSwitch` [AU:1351-1359]);
    - pull's `--ff-only` merge after fetch (`RebaseAfterFetch` [AU:2701-2712]);
    - the Sync dialog's ff-only merge [SYD:648-658].
  - There is no "Edit conflicts" post-command anywhere.

### 9.4 Merge-conflict info message (brief)

After a pull or merge that leaves unmerged entries, an information box is shown. `CMessageBox::ShowCheck` uses the registry value name `MergeConflictsNeedsCommit` for its "do not show again" checkbox. That setting can be reset on the Saved Data page. The box is shown by `DoPull` and `DoMerge` [AU:2335, 3299], and by the Sync dialog [SYD:292, 572; SSDP:377]. Its text is `IDS_NEED_TO_RESOLVE_CONFLICTS_HINT` [POT:10419]:

> "While merging, i.e. integrating changes of another (remote) branch into your local branch, a conflict in at least one file occurred. This means that you need to resolve this manually (i.e., you need to integrate your changes into a file which was also modified on another branch).\n\nAfter resolving all files, you need to perform a commit in order to complete the merge.\n\nIf you want to abort the merge, do a hard reset on HEAD or select abort merge on the context menu.\n\nSee help for more information."

In Sync, the "In conflict" tab is also filled with only UNMERGED rows and activated. [SYD:284-292]

### 9.5 Not established from source

- What `git checkout-index --temp` produces for a **symlink** stage on Windows (a link or a text file with the target), and so what the merge tool sees.
- How Edit conflicts behaves during a command-line `git rebase` using the `rebase-merge` backend (labels and swap). This is inferred in 9.1.6, not tested.
- Where git creates the intermediate `.merge_file_*` temp file before the move. It was assumed to be git's working directory (the repository root).
- How TortoiseGitMerge behaves when the base file is empty (added-by-both). Only the file preparation was traced.
- `DeleteConflictDlg`'s "Show log" button in a non-rebase merge uses `mineHash`, which `GetConflictTitles` sets only in the rebase branch. The log then probably opens with an empty revision; this was not verified.
- The Help topics (`HIDD_RESOLVE_CONFLICT`, `HIDD_RESOLVESUBMODULECONFLICT`, `HIDD_SETTINGSPROGSMERGE`) were not examined beyond the manual text cited.
