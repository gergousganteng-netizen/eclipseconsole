; Eclipse Console installer (Inno Setup 6).
;
; Build with tools/build_installer.ps1, which checks the payload is complete
; before calling ISCC -- a missing toolchain here produces an installer that
; installs cleanly and then cannot compile anything.

#define AppName    "Eclipse Console"
#define AppExe     "eclipse_console.exe"
#define AppVersion "1.11.0"
#define SrcDir     "..\build"

[Setup]
AppId={{7A3F9C21-4E8B-4E1D-9C36-2B5E0D9A1F84}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=Eclipse
DefaultDirName={autopf}\EclipseConsole
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=EclipseConsole-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Per-user install, and this is load-bearing rather than a preference.
;
; The app rewrites conf\offsets_windows.conf next to its own exe every time
; Roblox updates -- that is how the offset auto-fetch persists. Under Program
; Files a standard user cannot write there, the fetch fails, and because
; running is BLOCKED while the offsets are stale the app would be unusable
; without ever explaining why. {autopf} resolves to
; %LOCALAPPDATA%\Programs here, which is writable and needs no UAC prompt.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; ~320 MB of that is the bundled C++ compiler.
DirExistsWarning=no
DisableProgramGroupPage=yes
; Setup.exe's own icon. UninstallDisplayIcon below points at the app, which now
; carries the same icon as a compiled resource -- so installer, app, shortcuts
; and Add/Remove Programs all show one mark.
SetupIconFile=..\src\res\icon.ico
UninstallDisplayIcon={app}\{#AppExe}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#SrcDir}\eclipse_console.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\eclipse_cli.exe";          DestDir: "{app}"; Flags: ignoreversion

; Snippet DLLs import this; -static-libstdc++ does not pull it in. It must sit
; beside the EXE, because the host is what LoadLibrary's a snippet.
Source: "{#SrcDir}\libwinpthread-1.dll";      DestDir: "{app}"; Flags: ignoreversion

; A snippet links back against whichever binary loaded it, so both import
; libraries ship -- the CLI and the UI each export their own.
Source: "{#SrcDir}\libeclipse_console.dll.a"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\libeclipse_cli.dll.a";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#SrcDir}\fx.h";                     DestDir: "{app}"; Flags: ignoreversion

Source: "{#SrcDir}\web\*";  DestDir: "{app}\web";  Flags: ignoreversion recursesubdirs

; Seeded so the app has something to read before its first fetch. Not
; ignoreversion + never uninstalled below: after first run this file is the
; user's fetched offsets, not ours.
Source: "{#SrcDir}\conf\*"; DestDir: "{app}\conf"; Flags: onlyifdoesntexist

; The bundled toolchain. Without it nothing compiles on a machine that has no
; compiler installed, which is the entire point of shipping it.
Source: "{#SrcDir}\toolchain\*"; DestDir: "{app}\toolchain"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}";        Filename: "{app}\{#AppExe}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The offsets file is rewritten in place by the fetcher, so Inno no longer
; recognises it as something it installed and would leave it behind.
Type: files;      Name: "{app}\conf\offsets_windows.conf"
Type: dirifempty; Name: "{app}\conf"
; Compiler output and scratch files the app produced after install.
Type: filesandordirs; Name: "{app}\toolchain"

; NOTE: %APPDATA%\EclipseConsole is deliberately NOT touched here. It holds the
; user's snippets and tuned settings -- their own work, not ours to remove.
; Reinstalling should find everything exactly where they left it.

[Code]
// The WebView2 runtime draws the entire UI. Windows 11 always has it and
// current Windows 10 gets it with Edge, so this warns rather than blocks --
// refusing to install over a registry key that can be stale would be worse
// than letting someone with a working runtime through.
function WebView2Installed(): Boolean;
var
  Key: String;
begin
  Key := '{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  Result :=
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\' + Key) or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\' + Key) or
    RegKeyExists(HKEY_CURRENT_USER,  'Software\Microsoft\EdgeUpdate\Clients\' + Key);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not WebView2Installed() then
  begin
    if MsgBox('The Microsoft Edge WebView2 Runtime was not detected.' + #13#10#13#10 +
              'Eclipse Console draws its whole interface with it and will not start ' +
              'without it. It ships with Windows 11 and with current Windows 10.' + #13#10#13#10 +
              'Install anyway? You can add the runtime afterwards from ' +
              'go.microsoft.com/fwlink/p/?LinkId=2124703',
              mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;
