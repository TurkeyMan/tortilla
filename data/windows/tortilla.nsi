; Tortilla's Windows installer. Built by .github/ci/build-windows.ps1:
;   makensis -DVERSION=0.1 -DSTAGE=<install tree> -DOUT=<setup.exe> tortilla.nsi
; STAGE holds the app as installed: bin\ (tortilla.exe, self-contained, and
; the Explorer extension tortilla-shell.dll), and the README and licence.
;
; The extension is registered for every user (tortilla --register-shell
; --all-users). Explorer keeps it loaded, so it can't be overwritten or
; deleted while Explorer runs; it can be renamed, so an upgrade or uninstall
; moves the old one aside and deletes it at the next restart.

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
!insertmacro MUI_PAGE_LICENSE "${STAGE}/LICENSE"
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

; Moves a loaded Explorer extension out of the way and deletes it once
; nothing holds it (now, or at the next restart).
!macro MoveAsideShellExtension
  ${If} ${FileExists} "$INSTDIR\bin\tortilla-shell.dll"
    GetTempFileName $0 "$INSTDIR"
    Delete "$0"
    Rename "$INSTDIR\bin\tortilla-shell.dll" "$0"
    Delete /REBOOTOK "$0"
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
  ; A reinstall replaces the old tree rather than mixing DLLs of two versions.
  !insertmacro MoveAsideShellExtension
  RMDir /r /REBOOTOK "$INSTDIR\bin"
  SetOutPath "$INSTDIR"
  File /r "${STAGE}/*"
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

  ; Tortilla runs git itself; say so now rather than at first use.
  SearchPath $1 "git.exe"
  ${If} $1 == ""
    MessageBox MB_ICONINFORMATION "Tortilla uses Git for Windows, which isn't on the PATH.$\r$\nInstall it from https://git-scm.com before using Tortilla."
  ${EndIf}
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  ExecWait '"$INSTDIR\bin\tortilla.exe" --unregister-shell --all-users'
  !insertmacro MoveAsideShellExtension
  Delete "$SMPROGRAMS\Tortilla.lnk"
  RMDir /r /REBOOTOK "$INSTDIR\bin"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "Software\Tortilla"
SectionEnd
