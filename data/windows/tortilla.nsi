; Tortilla's Windows installer. Built by .github/ci/build-windows.ps1:
;   makensis -DVERSION=0.1 -DSTAGE=<install tree> -DOUT=<setup.exe> tortilla.nsi
; STAGE holds the app as installed: bin\ (tortilla.exe, self-contained, and
; the Explorer extension tortilla-shell.dll), translations\, and the README,
; licence and third-party notices.
;
; The extension is registered for every user (tortilla --register-shell
; --all-users). Explorer keeps it loaded, so it can't be overwritten or
; deleted while Explorer runs; it can be renamed, so an upgrade or uninstall
; moves the old one aside. Then Explorer is restarted (tortilla
; --restart-explorer, through Windows' Restart Manager, which starts it
; again as its user, not as Setup): the old extension is deleted while it's
; stopped, and the new one, with its badges, loads as it starts. No reboot.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "Tortilla"
OutFile "${OUT}"
InstallDir "$PROGRAMFILES64\Tortilla"
InstallDirRegKey HKLM "Software\Tortilla" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Tortilla"

!define MUI_ICON "${__FILEDIR__}/tortilla.ico"
!define MUI_UNICON "${__FILEDIR__}/tortilla.ico"
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0.0"
VIAddVersionKey "ProductName" "Tortilla"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "Tortilla installer"
VIAddVersionKey "LegalCopyright" "Manu Evans, GPL-3.0"

; Tortilla running from this install (a log, a dialog, git's password
; prompt) holds tortilla.exe, so it's closed before its files are replaced
; or removed: after asking (OK in a silent install), each window is asked
; to close as its own close button would; one still open after five
; seconds is ended. Copies of Tortilla elsewhere are left alone.
!define PS_TORTILLA `powershell.exe -NoProfile -NonInteractive -Command "Get-Process tortilla -ErrorAction SilentlyContinue | Where-Object { $$_.Path -eq '$INSTDIR\bin\tortilla.exe' }`
; The badges' status cache (tortilla.exe cache): no window and nothing to
; lose, so stopped without asking; Explorer's extension starts it again.
!define PS_TORTILLA_CACHE `powershell.exe -NoProfile -NonInteractive -Command "Get-CimInstance Win32_Process -Filter \"Name='tortilla.exe'\" | Where-Object { $$_.ExecutablePath -eq '$INSTDIR\bin\tortilla.exe' -and $$_.CommandLine -match ' cache$$' } | ForEach-Object { Stop-Process -Id $$_.ProcessId -Force }"`

Var Asked     ; the programs using Tortilla's files were listed, and closing them agreed
Var Leftover  ; the old extension, moved aside but still loaded, else ""

!macro CloseTortilla
  nsExec::Exec `${PS_TORTILLA_CACHE}`
  Pop $R1
  StrCpy $R0 0
  ${Do}
    ${IfNot} ${FileExists} "$INSTDIR\bin\tortilla.exe"
      ${Break}
    ${EndIf}
    ; A running program's file can't be opened for writing.
    ClearErrors
    FileOpen $R1 "$INSTDIR\bin\tortilla.exe" a
    ${IfNot} ${Errors}
      FileClose $R1
      ${Break}
    ${EndIf}
    ${If} $R0 == 0
      ${If} $Asked != 1
        MessageBox MB_ICONINFORMATION|MB_OKCANCEL "Tortilla is running. Setup will close its windows to continue;$\r$\nanything unsaved in them (a commit message, say) is lost." /SD IDOK IDOK +2
        Abort
      ${EndIf}
      nsExec::Exec `${PS_TORTILLA} | ForEach-Object { [void]$$_.CloseMainWindow() }"`
      Pop $R1
    ${ElseIf} $R0 == 10
      nsExec::Exec `${PS_TORTILLA} | Stop-Process -Force"`
      Pop $R1
    ${ElseIf} $R0 >= 20
      MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "Tortilla's files are still in use. Close Tortilla, then retry." /SD IDCANCEL IDRETRY +2
      Abort
      ; Retry: end them again, then wait.
      StrCpy $R0 9
    ${EndIf}
    IntOp $R0 $R0 + 1
    Sleep 500
  ${Loop}
!macroend

; Moves the Explorer extension out of the way, deleting it if nothing holds
; it; if Explorer has it loaded, $Leftover names it, for the restart.
!macro MoveAsideShellExtension
  StrCpy $Leftover ""
  ${If} ${FileExists} "$INSTDIR\bin\tortilla-shell.dll"
    GetTempFileName $Leftover "$INSTDIR"
    Delete "$Leftover"
    Rename "$INSTDIR\bin\tortilla-shell.dll" "$Leftover"
    Delete "$Leftover"
    ${IfNot} ${FileExists} "$Leftover"
      StrCpy $Leftover ""
    ${EndIf}
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Tortilla needs 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Tortilla"
  SetShellVarContext all
  ; An old extension a past install couldn't delete.
  Delete "$INSTDIR\ns*.tmp"
  ; Who has Tortilla's files open (Explorer, its windows), as the Restart
  ; Manager says, asked by the new tortilla.exe; said before anything closes.
  StrCpy $Asked 0
  ${If} ${FileExists} "$INSTDIR\bin\tortilla.exe"
    nsExec::Exec `${PS_TORTILLA_CACHE}`
    Pop $R1
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File "${STAGE}\bin\tortilla.exe"
    nsExec::ExecToStack '"$PLUGINSDIR\tortilla.exe" --in-use "$INSTDIR\bin"'
    Pop $R1
    Pop $R2
    ${If} $R1 == 0
    ${AndIf} $R2 != ""
      MessageBox MB_ICONINFORMATION|MB_OKCANCEL "These are using Tortilla's files:$\r$\n$\r$\n$R2$\r$\nSetup will close Tortilla's windows (anything unsaved in them, a commit message say, is lost)$\r$\nand restart Windows Explorer when it's done." /SD IDOK IDOK +2
      Abort
      StrCpy $Asked 1
    ${EndIf}
  ${EndIf}
  !insertmacro CloseTortilla
  ; A reinstall replaces the old tree rather than mixing DLLs of two versions.
  !insertmacro MoveAsideShellExtension
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\translations"
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Tortilla.lnk" "$INSTDIR\bin\tortilla.exe"

  ; Explorer's context menu, for every user.
  ExecWait '"$INSTDIR\bin\tortilla.exe" --register-shell --all-users' $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION "Tortilla's Explorer menu couldn't be registered."
  ${EndIf}

  WriteRegStr HKLM "Software\Tortilla" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "Tortilla"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "Manu Evans"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/TurkeyMan/tortilla"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\bin\tortilla.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1

  ; Explorer loads the extension, and its badges, as it starts: restarted
  ; now, which also lets go of the old one. Said beforehand if it was using
  ; the files; otherwise asked.
  StrCpy $R1 0
  ${If} $Asked == 1
  ${OrIf} $Leftover != ""
    StrCpy $R1 1
  ${ElseIf} ${Cmd} `MessageBox MB_ICONQUESTION|MB_YESNO "Restart Windows Explorer now, so Tortilla's badges show?$\r$\nIts windows close, and the taskbar is back in a moment. Otherwise they show after you next sign in." /SD IDYES IDYES`
    StrCpy $R1 1
  ${EndIf}
  ${If} $R1 == 1
    ExecWait '"$INSTDIR\bin\tortilla.exe" --restart-explorer "$Leftover"'
  ${EndIf}

  ; Tortilla runs git itself; say so now rather than at first use.
  SearchPath $1 "git.exe"
  ${If} $1 == ""
    MessageBox MB_ICONINFORMATION "Tortilla uses Git for Windows, which isn't on the PATH.$\r$\nInstall it from https://git-scm.com before using Tortilla."
  ${EndIf}
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  !insertmacro CloseTortilla
  ExecWait '"$INSTDIR\bin\tortilla.exe" --unregister-shell --all-users'
  !insertmacro MoveAsideShellExtension
  ; Explorer lets go of the extension as it restarts, and it's deleted then.
  ${If} $Leftover != ""
    ExecWait '"$INSTDIR\bin\tortilla.exe" --restart-explorer "$Leftover"'
  ${EndIf}
  Delete "$SMPROGRAMS\Tortilla.lnk"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\translations"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\NOTICES.md"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\ns*.tmp"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "Software\Tortilla"
SectionEnd
