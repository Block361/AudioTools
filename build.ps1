param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Test, [switch]$Package, [switch]$Wdm, [string]$WdkRoot = '', [string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$taskCmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($taskCmake) { $taskCmakePath = $taskCmake.Source }
else {
    $taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $taskVswhere)) { throw 'CMake is missing. Install Visual Studio with Desktop development with C++, or put CMake on PATH.' }
    $taskVsRoot = & $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $taskVsRoot) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
    $taskCmakePath = Join-Path $taskVsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
Push-Location $PSScriptRoot
try {
    $taskOptions = @('-S','.', '-B',$BuildDirectory)
    if ($WdkRoot) { $taskOptions += "-DOPENAOIP_WDK_ROOT=$WdkRoot" }
    & $taskCmakePath @taskOptions
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $taskCmakePath --build $BuildDirectory --config $Configuration --parallel 2
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    if ($Wdm) {
        & $taskCmakePath --build $BuildDirectory --config $Configuration --target OpenAoIPWdm --parallel 2
        if ($LASTEXITCODE -ne 0) { throw 'WDM compilation failed. Supply a matching WDK using -WdkRoot.' }
    }
    if ($Test) {
        $taskCtestPath = Join-Path (Split-Path $taskCmakePath) 'ctest.exe'
        & $taskCtestPath --test-dir $BuildDirectory -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Package) {
        $taskCpackPath = Join-Path (Split-Path $taskCmakePath) 'cpack.exe'
        & $taskCpackPath -C $Configuration --config (Join-Path $BuildDirectory 'CPackConfig.cmake') -B (Join-Path $BuildDirectory 'packages')
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
        if ($Wdm) {
            $taskWdk = if ($WdkRoot) { $WdkRoot } else { Join-Path $PSScriptRoot 'third_party/wdk/c' }
            & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'drivers/wdm/package.ps1') -BuildDirectory $BuildDirectory -Configuration $Configuration -WdkRoot $taskWdk
            if ($LASTEXITCODE -ne 0) { throw 'WDM development packaging failed.' }
        }
    }
    Write-Host "Controller: .\$BuildDirectory\$Configuration\OpenAoIP Controller.exe"
    Write-Host "Soundcard:  .\$BuildDirectory\$Configuration\OpenAoIP Virtual Soundcard.exe"
    Write-Host "Bridge:     .\$BuildDirectory\$Configuration\OpenAoIP Audio Bridge.exe"
    Write-Host "Suite installer: OpenAoIPSuiteSetup.exe; individual setups: Controller, VirtualSoundcard, AudioBridge"
} finally { Pop-Location }
