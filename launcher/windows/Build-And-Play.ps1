# Experimental local build; see docs/development/rom-bootstrap.md first.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$picker = New-Object System.Windows.Forms.OpenFileDialog
$picker.Title = 'Choose your RGO US ROM'
$picker.Filter = 'Big-endian Nintendo 64 ROM (*.z64)|*.z64'
if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }
$romPath = $picker.FileName
$picker.Dispose()
$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
& python (Join-Path $repoRoot 'scripts/build_from_rom.py') --rom $romPath --play
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Build stopped. Read the diagnostic path printed above.'
    exit $LASTEXITCODE
}
