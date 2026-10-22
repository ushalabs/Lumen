Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

Name "Lumen ${VERSION}"
Caption "Lumen ${VERSION} Setup — UshaLabs"
OutFile "${SETUP_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Lumen"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "InstallLocation"
BrandingText "Developed by UshaLabs"
Icon "${PROJECT_ROOT}\assets\icons\lumen.ico"
UninstallIcon "${PROJECT_ROOT}\assets\icons\lumen.ico"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Lumen"
VIAddVersionKey "CompanyName" "UshaLabs"
VIAddVersionKey "FileDescription" "Lumen — Lightweight Offline Intelligent Search for Windows"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 UshaLabs. All rights reserved."

ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Lumen by UshaLabs"
!define MUI_WELCOMEPAGE_TEXT "Welcome to the installation of Lumen — Lightweight Offline Intelligent Search for Windows.$\r$\n$\r$\nDeveloped by UshaLabs.$\r$\n$\r$\nLumen combines instant filename discovery, deep content search inside PDF, DOCX, and Markdown files, and 100% offline neural semantic search powered by embedded ONNX Runtime.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PROJECT_ROOT}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "Lumen Installation Complete"
!define MUI_FINISHPAGE_TEXT "Lumen has been installed on your PC.$\r$\n$\r$\nYou can launch Lumen immediately, or access it at any time from your Windows Desktop or with the global shortcut Ctrl+Alt+Space."
!define MUI_FINISHPAGE_RUN "$INSTDIR\Lumen.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Lumen now"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Lumen requires 64-bit Windows."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Lumen requires Windows 10 or newer."
    Abort
  ${EndIf}
FunctionEnd

Section "Lumen" Main
  SetOutPath "$INSTDIR"
  SetOverwrite on

  !include "${INSTALL_FILES}"

  ; Write uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Create shortcuts
  CreateDirectory "$SMPROGRAMS\UshaLabs"
  CreateShortcut "$SMPROGRAMS\UshaLabs\Lumen.lnk" "$INSTDIR\Lumen.exe" "" "$INSTDIR\Lumen.exe" 0
  CreateShortcut "$DESKTOP\Lumen.lnk" "$INSTDIR\Lumen.exe" "" "$INSTDIR\Lumen.exe" 0

  ; Register in Add/Remove Programs
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "DisplayName" "Lumen — Intelligent Search for Windows"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "Publisher" "UshaLabs"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "DisplayIcon" "$INSTDIR\Lumen.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; Kill running Lumen if any
  nsExec::ExecToStack 'taskkill /F /IM Lumen.exe'

  ; Remove shortcuts
  Delete "$SMPROGRAMS\UshaLabs\Lumen.lnk"
  RMDir "$SMPROGRAMS\UshaLabs"
  Delete "$DESKTOP\Lumen.lnk"

  ; Remove files
  !include "${UNINSTALL_FILES}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; Clean registry
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen"
SectionEnd
