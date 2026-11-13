; SPDX-License-Identifier: GPL-2.0-or-later
;
; The Windows installer of Terra Soar (NSIS 3, Unicode). Built by the
; packages workflow from the directory packaging/windows/deploy.py stages:
;
;   makensis /DVERSION=0.0.1 /DSTAGE=<stage> /DUNINSTALL_LIST=<file.nsh>
;            /DOUTFILE=<TerraSoar-0.0.1-setup.exe> packaging\windows\terrasoar.nsi
;
; Installs for all users into Program Files (administrator rights), with a
; Start menu entry, an optional desktop shortcut, the .terrasoar project
; type and an entry in Settings > Apps. Installing over an earlier version
; removes it first; users' data (data packs, saved tiles, projects) is
; never touched. The uninstaller removes exactly the installed files
; (UNINSTALL_LIST). Silent: setup.exe /S [/D=<folder>], uninstall.exe /S.
; See docs/release/packaging.md.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!ifndef VERSION
  !error "Pass /DVERSION=<x.y.z>"
!endif
!ifndef STAGE
  !error "Pass /DSTAGE=<staged installation>"
!endif
!ifndef UNINSTALL_LIST
  !error "Pass /DUNINSTALL_LIST=<uninstall include>"
!endif
!ifndef OUTFILE
  !define OUTFILE "TerraSoar-${VERSION}-setup.exe"
!endif

!define NAME "Terra Soar"
!define PUBLISHER "Peter Howkins"
!define HOMEPAGE "https://github.com/marutan/terrasoar"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\TerraSoar"
!define PROGID "TerraSoar.Project"

Name "${NAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${NAME}"
InstallDirRegKey HKLM "${UNINSTALL_KEY}" "InstallLocation"
BrandingText "${NAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${NAME}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${NAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "${PUBLISHER}. GNU GPL v2 or later."

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define MUI_ICON "${__FILEDIR__}\..\icons\terrasoar.ico"
!define MUI_UNICON "${__FILEDIR__}\..\icons\terrasoar.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\terrasoar.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\COPYING"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${NAME} needs 64-bit Windows 10 or later." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  ; Shortcuts for all users, as the program is.
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

; An earlier installation goes first (its uninstaller, silently, from where
; it is); users' data is not part of it.
Function RemovePrevious
  ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\uninstall.exe"
    DetailPrint "Removing the installed version in $0"
    ExecWait '"$0\uninstall.exe" /S _?=$0' $1
    Delete "$0\uninstall.exe"
    RMDir "$0"
  ${EndIf}
FunctionEnd

Section "${NAME}" SecMain
  SectionIn RO
  Call RemovePrevious
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortCut "$SMPROGRAMS\${NAME}.lnk" "$INSTDIR\terrasoar.exe"

  ; Project files open in Terra Soar.
  WriteRegStr HKLM "Software\Classes\.terrasoar" "" "${PROGID}"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "Terra Soar project"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\terrasoar.exe,0"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\terrasoar.exe" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ; Settings > Apps.
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "${HOMEPAGE}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\terrasoar.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  SectionGetSize ${SecMain} $0
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\${NAME}.lnk" "$INSTDIR\terrasoar.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The program, with its built-in data (the base pack)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut to ${NAME} on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; Exactly what was installed, then the folders it leaves empty.
  !include "${UNINSTALL_LIST}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${NAME}.lnk"
  Delete "$DESKTOP\${NAME}.lnk"

  ReadRegStr $0 HKLM "Software\Classes\.terrasoar" ""
  ${If} $0 == "${PROGID}"
    DeleteRegKey HKLM "Software\Classes\.terrasoar"
  ${EndIf}
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  DeleteRegKey HKLM "${UNINSTALL_KEY}"
SectionEnd
