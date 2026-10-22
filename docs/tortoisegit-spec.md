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
