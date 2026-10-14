[CmdletBinding()]
param(
    [string]$RomPath,
    [ValidatePattern('^[0-9a-f]{40}$')][string]$SourceCommit,
    [string]$DataRoot,
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# RGO: the launcher passes its portable data folder (data\ next to
# RGO-Launcher.exe); source, build cache and logs all stay in it.
$script:Portable = [bool]$DataRoot
$script:SetupRoot = if ($DataRoot) { [IO.Path]::GetFullPath($DataRoot) } else { Join-Path $env:LOCALAPPDATA 'JFGRecomp' }
$script:BuildRoot = Join-Path $script:SetupRoot 'b'
# scripts/build_paths.py allows 80 characters for <build root>\<12>\w\<12>\n.
$script:MaxBuildRoot = 80 - 30
# RGO: the launcher builds the RGO-Recomp revision it was packaged from.
$script:SourceRepository = 'https://github.com/ravenx444/RGO-Recomp.git'

function Refresh-SetupPath {
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' +
        [Environment]::GetEnvironmentVariable('PATH', 'User')
}

function Find-Git {
    $command = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $path = Join-Path $env:ProgramFiles 'Git\cmd\git.exe'
    if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    return $null
}

function Find-Python {
    $candidates = @()
    $command = Get-Command python.exe -ErrorAction SilentlyContinue
    # The WindowsApps alias can be an installed Store Python or an empty Store
    # shortcut. Only invoke it when the Python package actually exists.
    $storePython = @(Get-AppxPackage -Name 'PythonSoftwareFoundation.Python.*' -ErrorAction SilentlyContinue)
    if ($command -and ($command.Source -notlike '*\WindowsApps\*' -or $storePython.Count -gt 0)) {
        $candidates += $command.Source
    }
    foreach ($version in @('314', '313', '312', '311')) {
        $candidates += Join-Path $env:LOCALAPPDATA "Programs\Python\Python$version\python.exe"
        $candidates += Join-Path $env:ProgramFiles "Python$version\python.exe"
    }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            & $candidate -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) and sys.maxsize > 2**32 else 1)' 2>$null
            if ($LASTEXITCODE -eq 0) { return $candidate }
        }
    }
    return $null
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $found = @(& $vswhere -latest -products '*' -version '[17.0,18.0)' -requires `
        Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.CMake.Project `
        -property installationPath)
    if ($LASTEXITCODE -eq 0 -and $found.Count -gt 0) { return $found[0] }
    return $null
}

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    $tool = [IO.Path]::GetFileNameWithoutExtension($File).ToLowerInvariant()
    if ($tool -in @('git', 'python', 'python3', 'winget')) { Write-Output "JFG-SUPPORT setup_step=$tool" }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $File @Arguments 2>&1 | ForEach-Object { Write-Output ([string]$_) }
        $commandExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    if ($null -eq $commandExit) { throw 'Setup command did not start.' }
    Write-Output ('JFG-SUPPORT setup_exit=0x{0:x8}' -f $commandExit)
    if ($commandExit -in @(3010, 1641)) {
        Write-Output 'JFG-RESTART: Restart Windows, reopen this launcher and click Set up and build again.'
        exit 3010
    }
    if ($commandExit -ne 0) { throw "Setup command failed ($commandExit): $([IO.Path]::GetFileName($File)). See setup.log." }
}

function Install-Package([string]$Id, [string]$Override = '') {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if (-not $winget) { throw 'Install or update Microsoft App Installer from Microsoft Store, then reopen the launcher. Setup requires winget.' }
    Write-Output "Installing $Id. Approve the Windows installer prompt if shown."
    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
    if ($Override) { $arguments += @('--override', $Override) }
    Invoke-Checked $winget.Source $arguments
    Refresh-SetupPath
}

function Ensure-BuildTools {
    if (-not (Find-Git)) { Install-Package 'Git.Git' }
    if (-not (Find-Python)) { Install-Package 'Python.Python.3.12' }
    if (-not (Find-VisualStudio)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $existing = @()
        if (Test-Path -LiteralPath $vswhere) {
            $existing = @(& $vswhere -latest -products '*' -version '[17.0,18.0)' -property installationPath)
        }
        if ($existing.Count -gt 0) {
            Write-Output 'Adding C++ and CMake to Visual Studio 2022. Approve the Windows prompt.'
            $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
            $product = @(& $vswhere -latest -products '*' -version '[17.0,18.0)' -property productId)
            $workload = if ($product -contains 'Microsoft.VisualStudio.Product.BuildTools') { 'VCTools' } else { 'NativeDesktop' }
            $arguments = 'modify --installPath "' + $existing[0] + '" --add Microsoft.VisualStudio.Workload.' + $workload + ' --includeRecommended --passive --norestart'
            $process = Start-Process -FilePath $installer -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
            if ($process.ExitCode -in @(3010, 1641)) { Write-Output 'JFG-RESTART: Restart Windows, reopen the launcher and continue setup.'; exit 3010 }
            if ($process.ExitCode -ne 0) { throw 'Visual Studio setup did not finish. Reopen the launcher to retry.' }
        } else {
            Install-Package 'Microsoft.VisualStudio.2022.BuildTools' '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        }
    }
    if (-not (Find-Git) -or -not (Find-Python) -or -not (Find-VisualStudio)) {
        throw 'Build tools are incomplete. Restart Windows after any installer, then reopen the launcher.'
    }
}

function Get-PinnedSource([string]$Commit) {
    if ($Commit -notmatch '^[0-9a-f]{40}$') { throw 'The launcher has no valid source revision.' }
    $git = Find-Git
    $cache = [IO.Path]::GetFullPath((Join-Path $script:SetupRoot 'source'))
    # RGO (portable): one source folder, moved to the new revision on update.
    # The build cache is keyed by the source path, so a stable path lets an
    # update reuse the previous build instead of starting from zero.
    $destination = if ($script:Portable) { Join-Path $cache 'current' } else { Join-Path $cache $Commit }
    if ($script:Portable -and (Test-Path -LiteralPath $destination)) {
        $current = @(& $git -C $destination rev-parse HEAD)
        if ($LASTEXITCODE -eq 0 -and $current.Count -eq 1 -and $current[0] -ne $Commit) {
            $changes = @(& $git -C $destination status --porcelain)
            if ($LASTEXITCODE -ne 0 -or $changes.Count -ne 0) { throw 'Cached source has local changes. Existing files were preserved.' }
            Write-Output 'Updating the source to the version bundled with this launcher...'
            Invoke-Checked $git @('-C', $destination, 'fetch', '--depth=1', 'origin', $Commit)
            Invoke-Checked $git @('-C', $destination, 'checkout', '--detach', $Commit)
        }
    }
    if (-not (Test-Path -LiteralPath $destination)) {
        New-Item -ItemType Directory -Path $cache -Force | Out-Null
        $temporary = Join-Path $cache ('download-' + [Guid]::NewGuid().ToString('N'))
        Write-Output 'Downloading the source version bundled with this launcher...'
        Invoke-Checked $git @('init', $temporary)
        Invoke-Checked $git @('-C', $temporary, 'config', 'core.autocrlf', 'false')
        Invoke-Checked $git @('-C', $temporary, 'config', 'core.longpaths', 'true')
        Invoke-Checked $git @('-C', $temporary, 'remote', 'add', 'origin', $script:SourceRepository)
        Invoke-Checked $git @('-C', $temporary, 'fetch', '--depth=1', 'origin', $Commit)
        Invoke-Checked $git @('-C', $temporary, 'checkout', '--detach', $Commit)
        # Both resolved paths must stay inside the explicitly owned source cache.
        foreach ($path in @($temporary, $destination)) {
            if (-not [IO.Path]::GetFullPath($path).StartsWith($cache + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Source cache path escaped.' }
        }
        Move-Item -LiteralPath $temporary -Destination $destination
    }
    $head = @(& $git -C $destination rev-parse HEAD)
    if ($LASTEXITCODE -ne 0 -or $head.Count -ne 1 -or $head[0] -ne $Commit) { throw 'Cached source revision differs. Existing files were preserved.' }
    $changes = @(& $git -C $destination status --porcelain)
    if ($LASTEXITCODE -ne 0 -or $changes.Count -ne 0) { throw 'Cached source has local changes. Existing files were preserved.' }
    if (-not (Test-Path -LiteralPath (Join-Path $destination 'scripts\build_from_rom.py'))) { throw 'Downloaded source is incomplete.' }
    return $destination
}

function Invoke-Setup {
    if ($CheckOnly) {
        [ordered]@{ git = [bool](Find-Git); python = [bool](Find-Python);
            visualStudio = [bool](Find-VisualStudio); metadataBackend = 'native' } | ConvertTo-Json -Compress
        return
    }
    if (-not $SourceCommit -or -not $RomPath) { throw 'Choose a ROM through the launcher.' }
    # Direct invocation has the same no-download-before-ROM-validation boundary.
    if (-not (Test-Path -LiteralPath $RomPath -PathType Leaf) -or (Get-Item -LiteralPath $RomPath).Length -ne 33554432 -or
        (Get-FileHash -LiteralPath $RomPath -Algorithm SHA1).Hash -ne '493ced9008dbe932d6e91179b68e8630cf23a023') {
        throw 'Select the supported 32 MiB North American big-endian ROM.'
    }
    if ($script:Portable -and $script:BuildRoot.Length -gt $script:MaxBuildRoot) {
        throw ('The RGO folder is too deep for the build (Windows path limits). Move the whole folder somewhere ' +
            'with a short path, such as C:\Games\RGO, and open RGO-Launcher.exe from there.')
    }
    New-Item -ItemType Directory -Path $script:SetupRoot -Force | Out-Null
    Start-Transcript -Path (Join-Path $script:SetupRoot 'setup.log') -Append | Out-Null
    try {
        Write-Output 'JFG-SUPPORT stage=install-tools'
        Ensure-BuildTools
        # Keep status output visible without mixing it into the returned path.
        Write-Output 'JFG-SUPPORT stage=download-source'
        $source = Get-PinnedSource $SourceCommit | ForEach-Object {
            if ($_ -eq (Join-Path (Join-Path $script:SetupRoot 'source') $(if ($script:Portable) { 'current' } else { $SourceCommit }))) { $_ }
            else { Write-Host $_ }
        }
        $python = Find-Python
        Write-Output 'JFG-SUPPORT stage=build-game'
        Write-Output 'Building the game locally from your ROM. Compilation can take an hour or more. Leave this window open.'
        # The launcher is the frontend and it is open, so the build must not
        # rebuild it (build_from_rom.py refuses while the frontend is running).
        $build = @('-u', (Join-Path $source 'scripts\build_from_rom.py'), '--rom', $RomPath, '--no-frontend', '--metadata-backend', 'native')
        if ($script:Portable) { $build += @('--build-root', $script:BuildRoot) }
        Invoke-Checked $python $build
    } finally { Stop-Transcript | Out-Null }
}

if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-Setup }
    catch {
        Write-Output ('JFG-SUPPORT setup_error=' + $_.CategoryInfo.Category.ToString().ToLowerInvariant())
        [Console]::Error.WriteLine($_.Exception.Message); exit 1
    }
}
