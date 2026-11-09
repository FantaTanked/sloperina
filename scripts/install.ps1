<#
  sloperinaGZ installer for Windows.

  Run install.bat (or: powershell -ExecutionPolicy Bypass -File install.ps1). With no options it lists the
  emulators it finds and asks where to install. Options, for scripting:

    -Target eden|yuzu|suyu|sudachi|citron|torzu|ryujinx|switch|folder
    -Path   <folder>   the emulator's user folder (with -Target folder: a yuzu-family one), or the SD card's root
    -Uninstall         remove sloperinaGZ instead (your settings stay)
    -Yes               don't ask before replacing files

  It copies two files (the module, subsdk9, and the patch that loads it, <build id>.ips) and the translation
  template, into the layout each emulator or Atmosphère reads. Nothing else on your system is changed.
#>
param(
  [string]$Target = '',
  [string]$Path = '',
  [switch]$Uninstall,
  [switch]$Yes
)
$ErrorActionPreference = 'Stop'

$TitleId = '0400486024DEC000'
$Name = 'sloperinaGZ'
$BuildId = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Files = Join-Path $Here 'files'
$Module = Join-Path $Files 'subsdk9'
$Patch = Join-Path $Files "$BuildId.ips"
$Template = Join-Path $Files 'lang\template.json'

# emulators, with where they keep their data on Windows; kind: how they lay out mods
$Emulators = @(
  @{ Id = 'eden';    Label = 'Eden / Lichii'; Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'eden' },
  @{ Id = 'yuzu';    Label = 'yuzu';          Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'yuzu' },
  @{ Id = 'suyu';    Label = 'suyu';          Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'suyu' },
  @{ Id = 'sudachi'; Label = 'sudachi';       Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'sudachi' },
  @{ Id = 'citron';  Label = 'citron';        Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'citron' },
  @{ Id = 'torzu';   Label = 'torzu';         Kind = 'yuzu';    Dir = Join-Path $env:APPDATA 'torzu' },
  @{ Id = 'ryujinx'; Label = 'Ryujinx';       Kind = 'ryujinx'; Dir = Join-Path $env:APPDATA 'Ryujinx' }
)

function Say($text, $colour = 'Gray') { Write-Host $text -ForegroundColor $colour }

function Confirm-Replace($file) {
  if ($Yes -or -not (Test-Path $file)) { return $true }
  $answer = Read-Host "  $file already exists. Replace it? [Y/n]"
  return ($answer -eq '' -or $answer -match '^[yY]')
}

function Copy-One($from, $toDir, $toName) {
  New-Item -ItemType Directory -Force -Path $toDir | Out-Null
  $to = Join-Path $toDir $toName
  if (Confirm-Replace $to) {
    Copy-Item -Force $from $to
    Say "  copied  $to" 'Green'
  } else {
    Say "  skipped $to" 'Yellow'
  }
}

function Remove-IfThere($p, $why) {
  if (Test-Path $p) {
    Remove-Item -Recurse -Force $p
    Say "  removed $p$why" 'Yellow'
  }
}

# where each kind of target keeps the files
function Get-Layout($kind, $dir) {
  switch ($kind) {
    'yuzu' {
      return @{ Exefs = Join-Path $dir "load\$TitleId\$Name\exefs"; PatchDir = $null
                Lang = Join-Path $dir "sdmc\$Name\lang"; Old = @(Join-Path $dir "load\$TitleId\ootgz")
                Remove = @(Join-Path $dir "load\$TitleId\$Name") }
    }
    'ryujinx' {
      $t = $TitleId.ToLower()
      return @{ Exefs = Join-Path $dir "mods\contents\$t\$Name\exefs"; PatchDir = $null
                Lang = Join-Path $dir "sdcard\$Name\lang"; Old = @(Join-Path $dir "mods\contents\$t\ootgz")
                Remove = @(Join-Path $dir "mods\contents\$t\$Name") }
    }
    'switch' {
      return @{ Exefs = Join-Path $dir "atmosphere\contents\$TitleId\exefs"
                PatchDir = Join-Path $dir "atmosphere\exefs_patches\$Name"
                Lang = Join-Path $dir "$Name\lang"; Old = @(Join-Path $dir 'atmosphere\exefs_patches\ootgz')
                Remove = @((Join-Path $dir "atmosphere\exefs_patches\$Name"),
                           (Join-Path $dir "atmosphere\contents\$TitleId\exefs\subsdk9")) }
    }
  }
}

function Install-To($kind, $dir, $label) {
  $layout = Get-Layout $kind $dir
  Say "`nInstalling sloperinaGZ for $label in $dir" 'Cyan'
  foreach ($o in $layout.Old) { Remove-IfThere $o '  (an older ootgz install: two copies would clash)' }
  Copy-One $Module $layout.Exefs 'subsdk9'
  if ($layout.PatchDir) {
    Copy-One $Patch $layout.PatchDir "$BuildId.ips"
  } else {
    Copy-One $Patch $layout.Exefs "$BuildId.ips"
  }
  if (Test-Path $Template) { Copy-One $Template $layout.Lang 'template.json' }
  Say "`nDone. Start the game and press R + right stick down for the menu." 'Green'
  if ($kind -eq 'switch') {
    Say "Put the SD card back in the console and start the game with Atmosphere." 'Gray'
  }
}

function Uninstall-From($kind, $dir, $label) {
  $layout = Get-Layout $kind $dir
  Say "`nRemoving sloperinaGZ for $label from $dir" 'Cyan'
  foreach ($p in $layout.Remove + $layout.Old) { Remove-IfThere $p '' }
  Say "Done. Your settings (in the SD card's $Name folder) were kept; delete that folder to remove them too." 'Green'
}

foreach ($f in @($Module, $Patch)) {
  if (-not (Test-Path $f)) { Say "Missing $f. Run this from the unzipped release folder." 'Red'; exit 1 }
}

Say 'sloperinaGZ installer' 'Cyan'
Say 'For The Legend of Zelda: Ocarina of Time (Switch 2), update v131072.'

# choose the target
$kind = ''; $dir = ''; $label = ''
if ($Target -ne '') {
  $t = $Target.ToLower()
  $e = $Emulators | Where-Object { $_.Id -eq $t }
  if ($e) { $kind = $e.Kind; $dir = $e.Dir; $label = $e.Label }
  elseif ($t -eq 'switch') { $kind = 'switch'; $label = 'Nintendo Switch (Atmosphere)' }
  elseif ($t -eq 'folder') { $kind = 'yuzu'; $label = 'the emulator' }
  else { Say "Unknown target '$Target'." 'Red'; exit 1 }
  if ($Path -ne '') { $dir = $Path }
} else {
  $found = @($Emulators | Where-Object { Test-Path $_.Dir })
  $choices = @()
  foreach ($e in $found) { $choices += @{ Kind = $e.Kind; Dir = $e.Dir; Label = $e.Label; Text = "$($e.Label)  ($($e.Dir))" } }
  $choices += @{ Kind = 'switch'; Dir = ''; Label = 'Nintendo Switch (Atmosphere)'; Text = 'Nintendo Switch SD card (Atmosphere)' }
  $choices += @{ Kind = 'yuzu'; Dir = ''; Label = 'the emulator'; Text = 'Another emulator folder (yuzu-style: has load and sdmc)' }
  Say "`nWhere should it go?"
  for ($i = 0; $i -lt $choices.Count; $i++) { Say ("  {0}. {1}" -f ($i + 1), $choices[$i].Text) 'White' }
  $n = Read-Host "Number"
  if (-not ($n -match '^\d+$') -or [int]$n -lt 1 -or [int]$n -gt $choices.Count) { Say 'Nothing chosen.' 'Yellow'; exit 1 }
  $c = $choices[[int]$n - 1]
  $kind = $c.Kind; $dir = $c.Dir; $label = $c.Label
}
if ($dir -eq '') {
  if ($kind -eq 'switch') { $dir = Read-Host 'SD card (e.g. E:\)' }
  else { $dir = Read-Host "The emulator's user folder (the one with load and sdmc)" }
}
$dir = $dir.Trim('"').TrimEnd('\')
if ($dir -match '^[A-Za-z]:$') { $dir += '\' }
if (-not (Test-Path $dir)) { Say "$dir doesn't exist." 'Red'; exit 1 }
if ($kind -eq 'switch' -and -not (Test-Path (Join-Path $dir 'atmosphere')) -and -not $Yes) {
  $a = Read-Host "$dir has no atmosphere folder. Is it really the Switch's SD card? [y/N]"
  if (-not ($a -match '^[yY]')) { exit 1 }
}

if ($Uninstall) { Uninstall-From $kind $dir $label } else { Install-To $kind $dir $label }
