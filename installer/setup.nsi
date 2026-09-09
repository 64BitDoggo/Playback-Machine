; Playback Machine — NSIS installer script (NSIS 3.x).
;
; Build (the app must be built first, so dist/PlaybackMachine/ exists):
;     makensis installer/setup.nsi
;
; Produces: dist/PlaybackMachine-Setup.exe
;
; All paths are anchored to ${__FILEDIR__} (the folder containing this .nsi),
; so you can run makensis from any working directory.

!include "MUI2.nsh"

; This script lives in <root>/installer/, so the repo root is ..\
!define ROOT "${__FILEDIR__}\.."

Name "Playback Machine"
OutFile "${ROOT}\dist\PlaybackMachine-Setup.exe"
InstallDir "$PROGRAMFILES64\Playback Machine"
InstallDirRegKey HKLM "Software\PlaybackMachine" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; ---- UI -------------------------------------------------------------------
!define MUI_ICON "${ROOT}\resources\app.ico"
!define MUI_UNICON "${ROOT}\resources\app.ico"
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

  ; Application + FFmpeg DLLs (everything in the built folder).
  File /r "${ROOT}\dist\PlaybackMachine\*.*"

  WriteRegStr HKLM "Software\PlaybackMachine" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Start menu.
  CreateDirectory "$SMPROGRAMS\Playback Machine"
  CreateShortcut "$SMPROGRAMS\Playback Machine\Playback Machine.lnk" "$INSTDIR\PlaybackMachine.exe"
  CreateShortcut "$SMPROGRAMS\Playback Machine\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Desktop shortcut (default on).
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
