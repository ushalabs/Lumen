Unicode true
RequestExecutionLevel user
ManifestSupportedOS all
ManifestDPIAware true
ManifestDPIAwareness PerMonitorV2,System
SetCompressor /SOLID lzma
SetCompressorDictSize 32

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WinVer.nsh"

!define STARTUP_RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define STARTUP_VALUE "UshaLabs.Lumen"

Name "Lumen ${VERSION}"
Caption "Lumen ${VERSION} Setup - UshaLabs"
OutFile "${SETUP_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Lumen"
InstallDirRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen" "InstallLocation"
BrandingText "Developed by UshaLabs"
Icon "${PROJECT_ROOT}\assets\icons\lumen.ico"
UninstallIcon "${PROJECT_ROOT}\assets\icons\lumen.ico"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Lumen"
VIAddVersionKey "CompanyName" "UshaLabs"
VIAddVersionKey "FileDescription" "Lumen - Windows Setup Installer by UshaLabs"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 UshaLabs. All rights reserved."

ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Lumen by UshaLabs"
!define MUI_WELCOMEPAGE_TEXT "Welcome to the installation of Lumen - Lightweight Offline Intelligent Search for Windows.$\r$\n$\r$\nSole Developer: UshaLabs.$\r$\n$\r$\nLumen combines instant filename discovery across files of any size, deep content indexing for documents, spreadsheets, source code, and media metadata, and 100% offline semantic search powered by embedded ONNX Runtime.$\r$\n$\r$\nRuns quietly on startup in the Windows System Tray with its floating desktop icon and zero Taskbar footprint. All required C++ and Qt runtimes are bundled for Windows 10 and Windows 11 (x64)."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${PROJECT_ROOT}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "Lumen Installation Complete"
!define MUI_FINISHPAGE_TEXT "Lumen has been installed on your PC.$\r$\n$\r$\nSole Developer: UshaLabs.$\r$\n$\r$\nLumen is configured to start automatically on Windows bootup in the System Tray with its interactive desktop icon (never on the Taskbar), and can also be opened via Start Menu or Ctrl+Alt+Space."
!define MUI_FINISHPAGE_RUN "$INSTDIR\Lumen.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Lumen now"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var Isolated
Var RegistryKey
Var Shortcut
Var Arguments
Var Result
Var Output
Var InstallSizeKB

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Lumen requires 64-bit Windows 10 or Windows 11."
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Lumen requires Windows 10 or Windows 11 (x64)."
    Abort
  ${EndIf}

  SetRegView 64
  SetShellVarContext current
  StrCpy $Isolated "0"

  ${GetParameters} $Arguments
  ClearErrors
  ${GetOptions} $Arguments "/ISOLATEDTEST" $Result
  ${IfNot} ${Errors}
    StrCpy $Isolated "1"
  ${EndIf}
FunctionEnd

Section "Lumen" Main
  ${If} $Isolated != "1"
    ; Stop any running Lumen instance before upgrading files
    nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /IM Lumen.exe'
    Pop $Result
    Pop $Output
    ${If} $Result == "0"
      Sleep 500
    ${EndIf}
    nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /F /T /IM Lumen.exe'
    Pop $Result
    Pop $Output
    Sleep 250
  ${EndIf}

  SetOutPath "$INSTDIR"
  SetOverwrite on

  !include "${INSTALL_FILES}"

  ${GetSize} "$INSTDIR" "/S=0K" $InstallSizeKB $0 $1
  WriteINIStr "$INSTDIR\lumen-setup.ini" "Setup" "IsolatedTest" "$Isolated"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Keep WorkingDirectory anchored to $INSTDIR for all shortcuts
  SetOutPath "$INSTDIR"
  ${If} $Isolated == "1"
    StrCpy $RegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen.PackagingTest"
    CreateDirectory "$INSTDIR\StartMenu"
    StrCpy $Shortcut "$INSTDIR\StartMenu\Lumen.lnk"
    CreateShortcut "$Shortcut" "$INSTDIR\Lumen.exe" "" "$INSTDIR\Lumen.exe" 0
  ${Else}
    StrCpy $RegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen"
    CreateDirectory "$SMPROGRAMS\UshaLabs"
    StrCpy $Shortcut "$SMPROGRAMS\UshaLabs\Lumen.lnk"
    CreateShortcut "$Shortcut" "$INSTDIR\Lumen.exe" "" "$INSTDIR\Lumen.exe" 0
    CreateShortcut "$SMPROGRAMS\UshaLabs\Uninstall Lumen.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0

    ; Start Lumen automatically on Windows bootup in the system tray with desktop icon
    WriteRegStr HKCU "${STARTUP_RUN_KEY}" "${STARTUP_VALUE}" '$\"$INSTDIR\Lumen.exe$\" --startup'
    nsExec::ExecToStack '"$INSTDIR\Lumen.exe" --register-autostart'
    Pop $Result
    Pop $Output
  ${EndIf}

  ; Register in 64-bit Add/Remove Programs
  WriteRegStr HKCU "$RegistryKey" "DisplayName" "Lumen"
  WriteRegStr HKCU "$RegistryKey" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "$RegistryKey" "Publisher" "UshaLabs"
  WriteRegStr HKCU "$RegistryKey" "URLInfoAbout" "https://github.com/ushalabs/Lumen"
  WriteRegStr HKCU "$RegistryKey" "HelpLink" "https://github.com/ushalabs/Lumen"
  WriteRegStr HKCU "$RegistryKey" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "$RegistryKey" "DisplayIcon" "$INSTDIR\Lumen.exe,0"
  WriteRegStr HKCU "$RegistryKey" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
  WriteRegStr HKCU "$RegistryKey" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "$RegistryKey" "EstimatedSize" $InstallSizeKB
  WriteRegDWORD HKCU "$RegistryKey" "NoModify" 1
  WriteRegDWORD HKCU "$RegistryKey" "NoRepair" 1
SectionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext current
  ReadINIStr $Isolated "$INSTDIR\lumen-setup.ini" "Setup" "IsolatedTest"
  ${If} $Isolated == "1"
    StrCpy $RegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen.PackagingTest"
  ${Else}
    StrCpy $Isolated "0"
    StrCpy $RegistryKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen"
  ${EndIf}
FunctionEnd

Section "Uninstall"
  ${If} $Isolated != "1"
    nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /IM Lumen.exe'
    Pop $Result
    Pop $Output
    ${If} $Result == "0"
      Sleep 500
    ${EndIf}
    nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /F /T /IM Lumen.exe'
    Pop $Result
    Pop $Output
    Sleep 250
  ${EndIf}

  ; Remove shortcuts
  ${If} $Isolated == "1"
    Delete "$INSTDIR\StartMenu\Lumen.lnk"
    RMDir "$INSTDIR\StartMenu"
  ${Else}
    Delete "$SMPROGRAMS\UshaLabs\Lumen.lnk"
    Delete "$SMPROGRAMS\UshaLabs\Uninstall Lumen.lnk"
    RMDir "$SMPROGRAMS\UshaLabs"
    Delete "$DESKTOP\Lumen.lnk"

    ; Remove per-user sign-in scheduled task & autostart entry if it points to this installation
    ${If} ${FileExists} "$INSTDIR\Lumen.exe"
      nsExec::ExecToStack '"$INSTDIR\Lumen.exe" --unregister-autostart'
      Pop $Result
      Pop $Output
    ${EndIf}
    ReadRegStr $Result HKCU "${STARTUP_RUN_KEY}" "${STARTUP_VALUE}"
    ${If} $Result == '$\"$INSTDIR\Lumen.exe$\"'
      DeleteRegValue HKCU "${STARTUP_RUN_KEY}" "${STARTUP_VALUE}"
    ${ElseIf} $Result == '$\"$INSTDIR\Lumen.exe$\" --startup'
      DeleteRegValue HKCU "${STARTUP_RUN_KEY}" "${STARTUP_VALUE}"
    ${EndIf}
  ${EndIf}

  ; Remove installed files
  !include "${UNINSTALL_FILES}"
  Delete "$INSTDIR\lumen-setup.ini"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; Clean Add/Remove Programs registration if it belongs to this install location
  ReadRegStr $Result HKCU "$RegistryKey" "InstallLocation"
  ${If} $Result == "$INSTDIR"
    DeleteRegKey HKCU "$RegistryKey"
  ${EndIf}
SectionEnd
