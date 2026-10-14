$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'Setup.ps1')
$script:checks = 0
function Assert-Setup($Condition, [string]$Description) {
    if (-not $Condition) { throw $Description }
    $script:checks++
}
$probe = Join-Path ([IO.Path]::GetTempPath()) ('jfg-stderr-' + [Guid]::NewGuid().ToString('N') + '.ps1')
[IO.File]::WriteAllText($probe, "[Console]::Error.WriteLine('fixture native stderr'); exit 7")
try {
    $script:nativeProbe = @()
    $rejected = $false
    try { Invoke-Checked "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" @('-NoProfile', '-File', $probe) | ForEach-Object { $script:nativeProbe += $_ } }
    catch { $rejected = $_.Exception.Message -like '*failed (7)*' }
    Assert-Setup $rejected 'Native stderr must preserve the actual command failure.'
    Assert-Setup ($script:nativeProbe -contains 'JFG-SUPPORT setup_exit=0x00000007') 'Native exit code was not included in diagnostics.'
} finally { Remove-Item -LiteralPath $probe }
function Find-Git { if ($script:hasGit) { 'synthetic-git' } }
function Find-Python { if ($script:hasPython) { 'synthetic-python' } }
function Find-VisualStudio { if ($script:hasVs) { 'synthetic-vs' } }
function Test-Ubuntu { throw 'Windows-only setup invoked Ubuntu' }
function Test-LinuxPackages { throw 'Windows-only setup invoked Linux packages' }
function Install-Package([string]$Id, [string]$Override = '') {
    $script:installs += $Id
    switch ($Id) {
        'Git.Git' { $script:hasGit = $true }
        'Python.Python.3.12' { $script:hasPython = $true }
        default { throw 'Unexpected installer in fixture' }
    }
}
function Invoke-Checked([string]$File, [string[]]$Arguments) {
    $script:commands += ,$Arguments
    if ($File -match 'wsl' -or $Arguments -contains 'apt-get') { throw 'Linux installer was invoked' }
}
$script:hasGit = $true; $script:hasPython = $true; $script:hasVs = $true; $script:hasLinux = $true
$script:installs = @(); $script:commands = @()
Ensure-BuildTools
Assert-Setup ($script:installs.Count -eq 0 -and $script:commands.Count -eq 0) 'A ready machine must not install anything.'
$script:hasGit = $false; $script:hasPython = $false; $script:hasLinux = $false
Ensure-BuildTools
Assert-Setup (($script:installs -join ',') -eq 'Git.Git,Python.Python.3.12') 'Only missing Windows tools should be installed.'
Assert-Setup ($script:commands.Count -eq 0) 'Windows-only setup must not install Linux packages.'
$before = $script:installs.Count + $script:commands.Count
Ensure-BuildTools
Assert-Setup (($script:installs.Count + $script:commands.Count) -eq $before) 'Retry must reuse installed components.'
$CheckOnly = $true
$readiness = Invoke-Setup | ConvertFrom-Json
Assert-Setup ($readiness.metadataBackend -eq 'native') 'Readiness selected the wrong generation backend.'
Assert-Setup ($null -eq $readiness.PSObject.Properties['ubuntu']) 'Readiness still requires Ubuntu.'
Assert-Setup ($null -eq $readiness.PSObject.Properties['linuxPackages']) 'Readiness still requires Linux packages.'
$CheckOnly = $false; $SourceCommit = 'a' * 40
$RomPath = Join-Path ([IO.Path]::GetTempPath()) ('jfg-invalid-' + [Guid]::NewGuid().ToString('N') + '.z64')
[IO.File]::WriteAllBytes($RomPath, [byte[]]@(1,2,3,4))
try {
    $rejected = $false
    try { Invoke-Setup } catch { $rejected = $_.Exception.Message -like '*supported 32 MiB*' }
    Assert-Setup $rejected 'Setup must reject an unsupported ROM before installation.'
    Assert-Setup (($script:installs.Count + $script:commands.Count) -eq $before) 'Bad ROM triggered a download.'
    $rejected = $false
    try { Get-PinnedSource '../main' } catch { $rejected = $_.Exception.Message -like '*source revision*' }
    Assert-Setup $rejected 'Mutable or path-like source revisions must be rejected.'
} finally { Remove-Item -LiteralPath $RomPath }
# A complete first-run flow records the actual Windows build command. The
# synthetic ROM's identity is supplied only by this fixture, never production.
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('rgo-native-setup-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
$RomPath = Join-Path $fixtureRoot 'synthetic.z64'
$stream = [IO.File]::Create($RomPath)
try { $stream.SetLength(33554432) } finally { $stream.Dispose() }
function Get-FileHash { [pscustomobject]@{ Hash = '493ced9008dbe932d6e91179b68e8630cf23a023' } }
function Start-Transcript { }
function Stop-Transcript { }
function Get-PinnedSource([string]$Commit) { Join-Path (Join-Path $script:SetupRoot 'source') $Commit }
$script:Portable = $false
$script:SetupRoot = Join-Path $fixtureRoot 'data'
$script:BuildRoot = Join-Path $script:SetupRoot 'b'
$SourceCommit = 'a' * 40
$CheckOnly = $false
$script:commands = @()
try {
    Invoke-Setup | Out-Null
    Assert-Setup ($script:commands.Count -eq 1) 'First-run setup must invoke just the Windows build after tools are ready.'
    $buildArguments = $script:commands[0]
    $backendIndex = [Array]::IndexOf($buildArguments, '--metadata-backend')
    Assert-Setup ($backendIndex -ge 0 -and $buildArguments[$backendIndex + 1] -eq 'native') 'First-run setup must explicitly choose native metadata.'
    Assert-Setup ($buildArguments -contains '--no-frontend') 'First-run setup must preserve the open launcher.'
    Assert-Setup ($buildArguments -contains $RomPath) 'First-run setup must use only the selected ROM.'
    Assert-Setup ($script:installs -notcontains 'Canonical.Ubuntu') 'First-run setup requested a Linux installer.'
} finally {
    $resolvedFixture = [IO.Path]::GetFullPath($fixtureRoot)
    $temporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolvedFixture.StartsWith($temporaryParent, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped temporary storage.' }
    Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output "Setup checks passed: $script:checks"
