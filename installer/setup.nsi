; Playback Machine — NSIS installer script.
;
; Build (from the repo root, after `scripts/build_cross.sh` has produced
; dist/PlaybackMachine/):
;   makensis installer/setup.nsi
;
; Produces: dist/PlaybackMachine-Setup.exe

!include "MUI2.nsh"

Name "Playback Machine"
OutFile "dist/PlaybackMachine-Setup.exe"
InstallDir "$PROGRAMFILES64\Playback Machine"
InstallDirRegKey HKLM "Software\PlaybackMachine" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; ---- UI -------------------------------------------------------------------
!define MUI_ICON "..\resources\app.ico"
!define MUI_UNICON "..\resources\app.ico"
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\PlaybackMachine.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---- Install --------------------------------------------------------------
Section "Playback Machine" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; Application + FFmpeg DLLs.
  File /r "..\dist\PlaybackMachine\*.*"

  WriteRegStr HKLM "Software\PlaybackMachine" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Start menu.
  CreateDirectory "$SMPROGRAMS\Playback Machine"
  CreateShortcut "$SMPROGRAMS\Playback Machine\Playback Machine.lnk" "$INSTDIR\PlaybackMachine.exe"
  CreateShortcut "$SMPROGRAMS\Playback Machine\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Desktop shortcut (optional, default on).
  CreateShortcut "$DESKTOP\Playback Machine.lnk" "$INSTDIR\PlaybackMachine.exe"
SectionEnd

; ---- Uninstall ------------------------------------------------------------
Section "Uninstall"
  Delete "$DESKTOP\Playback Machine.lnk"
  Delete "$SMPROGRAMS\Playback Machine\Playback Machine.lnk"
  Delete "$SMPROGRAMS\Playback Machine\Uninstall.lnk"
  RMDir  "$SMPROGRAMS\Playback Machine"

  DeleteRegKey HKLM "Software\PlaybackMachine"

  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\uninstall.exe"
  RMDir /r "$INSTDIR"
SectionEnd
