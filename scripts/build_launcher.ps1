[CmdletBinding()]
param([switch]$Test, [string]$NativeBuildDirectory, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$launcherRepoRoot = Split-Path -Parent $PSScriptRoot
$launcherOutput = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $launcherRepoRoot 'build\launcher' }
$launcherCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $launcherCompiler -PathType Leaf)) {
    throw 'The Windows x64 .NET Framework compiler is required.'
}
New-Item -ItemType Directory -Path $launcherOutput -Force | Out-Null
$launcherSource = Join-Path $launcherRepoRoot 'launcher\windows\Launcher.cs'
$launcherExe = Join-Path $launcherOutput 'RGO-Launcher.exe'
$launcherHelper = Join-Path $launcherOutput 'JFG-Setup.exe'
$launcherFirstRun = Join-Path $launcherRepoRoot 'launcher\windows\FirstRun.cs'
$launcherSetup = Join-Path $launcherRepoRoot 'launcher\windows\Setup.ps1'
$launcherCommit = (& git -C $launcherRepoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $launcherCommit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot pin launcher source revision.' }
$launcherBuildInfo = Join-Path $launcherOutput 'BuildInfo.cs'
[IO.File]::WriteAllText($launcherBuildInfo, 'namespace JfgLauncher { internal static class BuildInfo { internal const string SourceCommit = "' + $launcherCommit + '"; } }')
$launcherSources = @($launcherSource, $launcherFirstRun, $launcherBuildInfo, (Join-Path $launcherRepoRoot 'launcher/windows/FrontendBridge.cs'),
    (Join-Path $launcherRepoRoot 'launcher/windows/Support.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/Diagnostics.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/Controller.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NavigationMap.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/Audio.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/MapLayers.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NavigationRoute.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NavigationExplorer.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NavigationCollision.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/BoxJump.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/ChestRoute.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NavigationRunner.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/AutonomousExplorer.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/InventoryWindow.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/InventoryImages.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/InventoryModel.cs'), (Join-Path $launcherRepoRoot 'launcher/windows/NativeLiveTools.cs'))
$launcherIcon = Join-Path $launcherRepoRoot 'launcher/ui/assets/app.ico'
$launcherCommon = @("/win32icon:$launcherIcon", "/resource:$launcherIcon,JfgLauncher.AppIcon", '/nologo', '/optimize+', '/debug-', '/platform:x64', '/warnaserror+',
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
    '/reference:System.Runtime.Serialization.dll', '/reference:System.IO.Compression.dll', "/resource:$launcherSetup,JfgLauncher.Setup.ps1", "/resource:$(Join-Path $launcherRepoRoot 'launcher/ui/fonts/Barlow-Regular.ttf'),JfgLauncher.Barlow.Regular")
& $launcherCompiler @launcherCommon '/target:winexe' "/out:$launcherHelper" @launcherSources
if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed.' }
if ($Test) {
    $launcherTests = Join-Path $launcherOutput 'LauncherTests.exe'
    $launcherTestSource = Join-Path $launcherRepoRoot 'launcher\windows\LauncherTests.cs'
    & $launcherCompiler @launcherCommon '/target:exe' '/main:JfgLauncher.LauncherTests' "/out:$launcherTests" @launcherSources $launcherTestSource (Join-Path $launcherRepoRoot 'launcher/windows/NavigationExplorerTests.cs') (Join-Path $launcherRepoRoot 'launcher/windows/NavigationCollisionTests.cs') (Join-Path $launcherRepoRoot 'launcher/windows/BoxJumpTests.cs') (Join-Path $launcherRepoRoot 'launcher/windows/AutonomousExplorerTests.cs') (Join-Path $launcherRepoRoot 'launcher/windows/UnifiedNavigationTests.cs')
    if ($LASTEXITCODE -ne 0) { throw 'Launcher test compilation failed.' }
    & $launcherTests (Join-Path $launcherOutput 'launcher-preview.png')
    if ($LASTEXITCODE -ne 0) { throw 'Launcher tests failed.' }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $launcherRepoRoot 'launcher\windows\SetupTests.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'First-run setup tests failed.' }
}

# Native window shell with embedded setup/settings services.
$frontendVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$frontendVs = (& $frontendVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $frontendVs) { throw 'Visual Studio C++ tools are required to build the native launcher.' }
$frontendCmake = Join-Path $frontendVs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$frontendBuild = if ($NativeBuildDirectory) { [IO.Path]::GetFullPath($NativeBuildDirectory) } else { Join-Path $launcherOutput 'native' }
& $frontendCmake -S (Join-Path $launcherRepoRoot 'launcher/native') -B $frontendBuild -G 'Visual Studio 17 2022' -A x64 "-DJFG_FRONTEND_HELPER=$launcherHelper"
if ($LASTEXITCODE -ne 0) { throw 'Native frontend configuration failed.' }
& $frontendCmake --build $frontendBuild --config Release --target RGO-Launcher --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native frontend compilation failed.' }
Copy-Item -LiteralPath (Join-Path $frontendBuild 'Release/RGO-Launcher.exe') -Destination $launcherExe -Force

$launcherInputs = [ordered]@{}
foreach ($relative in @('launcher/windows/Launcher.cs', 'launcher/windows/FrontendBridge.cs', 'src/app/frontend_win32.cpp', 'launcher/native/CMakeLists.txt', 'launcher/windows/FirstRun.cs',
        'launcher/windows/Setup.ps1', 'launcher/windows/Support.cs', 'launcher/windows/Diagnostics.cs', 'launcher/windows/Controller.cs', 'launcher/windows/NavigationMap.cs', 'launcher/windows/Audio.cs', 'launcher/windows/MapLayers.cs', 'launcher/windows/NavigationRoute.cs', 'launcher/windows/NavigationExplorer.cs', 'launcher/windows/NavigationCollision.cs', 'launcher/windows/BoxJump.cs', 'launcher/windows/ChestRoute.cs', 'launcher/windows/NavigationRunner.cs', 'launcher/windows/AutonomousExplorer.cs', 'launcher/windows/InventoryWindow.cs', 'launcher/windows/InventoryImages.cs', 'launcher/windows/InventoryModel.cs', 'launcher/windows/NativeLiveTools.cs', 'scripts/build_launcher.ps1')) {
    $launcherInputs[$relative] = (Get-FileHash -LiteralPath (Join-Path $launcherRepoRoot $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
Get-ChildItem -LiteralPath (Join-Path $launcherRepoRoot 'launcher/ui') -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($launcherRepoRoot.Length + 1).Replace('\', '/')
    $launcherInputs[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$launcherReceipt = [ordered]@{ source_commit = $launcherCommit;
    executable_sha256 = (Get-FileHash -LiteralPath $launcherExe -Algorithm SHA256).Hash.ToLowerInvariant(); inputs = $launcherInputs }
[IO.File]::WriteAllText((Join-Path $launcherOutput 'launcher-build.json'), ($launcherReceipt | ConvertTo-Json -Depth 4))
Write-Output ('Launcher built: ' + $launcherExe)
