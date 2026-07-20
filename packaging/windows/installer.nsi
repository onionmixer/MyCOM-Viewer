Unicode true
!include "MUI2.nsh"

!ifndef STAGE_DIR
  !error "STAGE_DIR must point to the CMake install staging directory"
!endif
!ifndef OUTFILE
  !error "OUTFILE must name the generated installer"
!endif

Name "MYCOM Viewer"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\MYCOM Viewer"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\mycom-viewer.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run MYCOM Viewer"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\share\doc\mycom-viewer\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open README"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "MYCOM Viewer" SecMain
  SetOutPath "$INSTDIR"
  File /r "${STAGE_DIR}\*.*"
  CreateDirectory "$SMPROGRAMS\MYCOM Viewer"
  CreateShortCut "$SMPROGRAMS\MYCOM Viewer\MYCOM Viewer.lnk" "$INSTDIR\bin\mycom-viewer.exe"
  CreateShortCut "$SMPROGRAMS\MYCOM Viewer\README.lnk" "$INSTDIR\share\doc\mycom-viewer\README.txt"
  CreateShortCut "$SMPROGRAMS\MYCOM Viewer\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MYCOM Viewer" "DisplayName" "MYCOM Viewer"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MYCOM Viewer" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\MYCOM Viewer\MYCOM Viewer.lnk"
  Delete "$SMPROGRAMS\MYCOM Viewer\README.lnk"
  Delete "$SMPROGRAMS\MYCOM Viewer\Uninstall.lnk"
  RMDir "$SMPROGRAMS\MYCOM Viewer"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MYCOM Viewer"
  RMDir /r "$INSTDIR"
SectionEnd
