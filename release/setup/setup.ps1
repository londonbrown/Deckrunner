# Deckrunner setup: finds Python (or offers SK8V's pinned download), finds Cyberpunk 2077 (Steam or GOG, else asks),
# asks for the player's own Skate 3 disc image or extracted folder, and runs prepare.py. Started by "Deckrunner Setup.cmd".
#   powershell -ExecutionPolicy Bypass -File setup\setup.ps1 [-Uninstall]
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$Here = $PSScriptRoot
$Bundled = Join-Path (Split-Path -Parent $Here) 'python\python.exe'  # where get-python.ps1 puts it

function Say($text) { Write-Host $text -ForegroundColor Cyan }
function Fail($text) { Write-Host "`n$text" -ForegroundColor Red; Read-Host 'Press Enter to close'; exit 1 }

function Test-Python($exe) {
  if (-not $exe) { return $false }
  $ErrorActionPreference = 'Continue'
  & $exe -s -c "import sys, numpy, PIL; sys.exit(0 if sys.version_info >= (3, 11) else 1)" 2>$null | Out-Null
  $ok = $LASTEXITCODE -eq 0
  $ErrorActionPreference = 'Stop'
  return $ok
}

function Find-Python {
  if (Test-Path $Bundled) { $env:PYTHONHOME = Split-Path $Bundled; $env:PYTHONPATH = $null; $env:PYTHONNOUSERSITE = '1'; return $Bundled }
  foreach ($c in @('py', 'python')) {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }
    $exe = if ($c -eq 'py') { (& py -3 -c "import sys; print(sys.executable)" 2>$null) } else { $cmd.Source }
    if (Test-Python $exe) { return $exe }
  }
  $answer = [Windows.Forms.MessageBox]::Show(
    "Setup needs Python 3.11 or newer with numpy and Pillow, and none was found.`n`nDownload a private copy now (about 31 MB from python.org and PyPI, each file checked against its published SHA-256)? Nothing is installed system-wide.",
    'Deckrunner setup', 'YesNo', 'Question')
  if ($answer -ne 'Yes') { Fail 'Setup needs Python. Install Python 3.11+ with numpy and Pillow, or run setup again and let it download one.' }
  & powershell -ExecutionPolicy Bypass -File (Join-Path $Here 'get-python.ps1')
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Bundled)) { Fail 'The Python download did not finish.' }
  $env:PYTHONHOME = Split-Path $Bundled; $env:PYTHONPATH = $null; $env:PYTHONNOUSERSITE = '1'
  return $Bundled
}

function Find-Cyberpunk {
  $found = @()
  try {
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) { $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }) }
    foreach ($l in $libs) { $found += Join-Path $l 'steamapps\common\Cyberpunk 2077' }
  } catch {}
  foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1423049311', 'HKLM:\SOFTWARE\GOG.com\Games\1423049311')) {
    try { $found += (Get-ItemProperty $k -ErrorAction Stop).path } catch {}
  }
  foreach ($f in $found) { if ($f -and (Test-Path (Join-Path $f 'bin\x64\Cyberpunk2077.exe'))) { return $f } }
  return $null
}

Write-Host "`n  DECKRUNNER  -  Skate 3 in Cyberpunk 2077`n" -ForegroundColor Red
$python = Find-Python
Say "Python: $python"

$cp = Find-Cyberpunk
if ($cp) {
  $use = [Windows.Forms.MessageBox]::Show("Install into this Cyberpunk 2077 folder?`n`n$cp", 'Deckrunner setup', 'YesNoCancel', 'Question')
  if ($use -eq 'Cancel') { exit 1 }
  if ($use -eq 'No') { $cp = $null }
}
if (-not $cp) {
  $d = New-Object Windows.Forms.FolderBrowserDialog
  $d.Description = 'Choose your Cyberpunk 2077 folder (the one holding bin\x64\Cyberpunk2077.exe)'
  if ($d.ShowDialog() -ne 'OK') { exit 1 }
  $cp = $d.SelectedPath
}
if (-not (Test-Path (Join-Path $cp 'bin\x64\Cyberpunk2077.exe'))) { Fail "$cp is not a Cyberpunk 2077 folder." }
Say "Cyberpunk 2077: $cp"

if ($Uninstall) {
  & $python -B (Join-Path $Here 'prepare.py') --cyberpunk $cp --uninstall
  Read-Host 'Press Enter to close'; exit $LASTEXITCODE
}

$f = New-Object Windows.Forms.OpenFileDialog
$f.Title = 'Choose your Skate 3 (Xbox 360) disc image, or Cancel to pick an extracted disc folder'
$f.Filter = 'Disc image (*.iso)|*.iso|All files (*.*)|*.*'
if ($f.ShowDialog() -eq 'OK') { $skate = $f.FileName }
else {
  $d = New-Object Windows.Forms.FolderBrowserDialog
  $d.Description = 'Choose your extracted Skate 3 disc folder (the one holding default.xex)'
  if ($d.ShowDialog() -ne 'OK') { exit 1 }
  $skate = $d.SelectedPath
}
Say "Skate 3: $skate"
Say "`nSetting up. The first run downloads Deckrunner's setup tools (0.3 MB) and WolvenKit (45 MB), and .NET 10 (37 MB) if you don't have it;`nall from their official sources, each checked against its published hash. A rerun picks up where it stopped.`n"
& $python -B (Join-Path $Here 'prepare.py') --skate $skate --cyberpunk $cp --clean
if ($LASTEXITCODE -ne 0) { Fail 'Setup stopped (see above). Fix what it says and run setup again: it picks up where it stopped.' }
Write-Host "`nDone. Start the game, load a save, and press Ctrl+Shift+F10 to skate." -ForegroundColor Green
Read-Host 'Press Enter to close'
