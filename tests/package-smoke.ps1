param([Parameter(Mandatory=$true)][string]$BinaryDirectory)
$ErrorActionPreference = 'Stop'
$BinaryDirectory = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$taskRoot = Join-Path ([IO.Path]::GetTempPath()) ('OpenAoIP-package-test-' + [guid]::NewGuid().ToString('N'))
foreach ($taskKind in @('Controller','VirtualSoundcard','AudioBridge','Suite')) {
    $taskFolder = Join-Path $taskRoot $taskKind
    $taskProcess = Start-Process -FilePath (Join-Path $BinaryDirectory "OpenAoIP${taskKind}Setup.exe") -ArgumentList @('--extract',('"'+$taskFolder+'"')) -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(10000)) { Stop-Process -Id $taskProcess.Id -Force; throw "Installer extraction timed out: $taskKind" }
    if ($taskProcess.ExitCode -ne 0) { throw "Installer extraction failed: $taskKind" }
    $taskExecutable = switch($taskKind) { 'Controller' { 'OpenAoIP Controller.exe' } 'VirtualSoundcard' {'OpenAoIP Virtual Soundcard.exe'} 'AudioBridge' {'OpenAoIP Audio Bridge.exe'} 'Suite' {'OpenAoIP Controller.exe'} }
    if ((Get-FileHash -LiteralPath (Join-Path $taskFolder $taskExecutable)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory $taskExecutable)).Hash) { throw 'Extracted application differs from built application' }
    foreach ($taskName in @('WINDOWS-START.md','LICENSE','THIRD_PARTY.md','licenses/LICENSE-miniaudio.txt')) { if (-not (Test-Path -LiteralPath (Join-Path $taskFolder $taskName))) { throw "Missing package file: $taskName" } }
    if ($taskKind -ne 'Controller') { if((Get-FileHash -LiteralPath (Join-Path $taskFolder 'OpenAoIP Engine.exe')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory 'OpenAoIP Engine.exe')).Hash){ throw 'Engine payload differs' } }
    if ($taskKind -eq 'Suite') { foreach($taskApp in @('OpenAoIP Controller.exe','OpenAoIP Virtual Soundcard.exe','OpenAoIP Audio Bridge.exe')) { if((Get-FileHash -LiteralPath (Join-Path $taskFolder $taskApp)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory $taskApp)).Hash) {throw 'Suite payload differs'} } }
    if ($taskKind -eq 'VirtualSoundcard' -or $taskKind -eq 'Suite') {
        if (-not (Test-Path -LiteralPath (Join-Path $taskFolder 'OpenAoIPWdmSetup.exe'))) { throw 'WDM signed-package installer helper missing' }
        if ((Get-FileHash -LiteralPath (Join-Path $taskFolder 'OpenAoIPASIO64.dll')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $BinaryDirectory 'OpenAoIPASIO64.dll')).Hash) { throw 'ASIO payload differs' }
        foreach ($taskName in @('asio-source/COPYING','asio-source/SDK-LICENSE.txt','asio-source/CMakeLists.txt','asio-source/driver/openaoip_asio.cpp','asio-source/include/aoip/virtual_bridge.hpp','asio-source/sdk/iasiodrv.h')) { if (-not (Test-Path -LiteralPath (Join-Path $taskFolder $taskName))) { throw "Missing ASIO corresponding source: $taskName" } }
    }
}
Write-Output "PASS independent installer extraction, binary hashes, licenses and complete ASIO source payload. Extracted under $taskRoot"
