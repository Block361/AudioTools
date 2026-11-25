param([string]$BuildDirectory='build/desktop',[string]$Configuration='Release',[string]$WdkRoot='third_party/wdk/c')
$ErrorActionPreference='Stop'
$taskProject = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $taskProject
try {
    $taskBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
    $taskWdk = (Resolve-Path -LiteralPath $WdkRoot).Path
    $taskPackage = Join-Path $taskBuild 'wdm-package-unsigned'
    New-Item -ItemType Directory -Force -Path $taskPackage | Out-Null
    $taskFiles = @('OpenAoIPWdm.sys','OpenAoIPWdm.inf','OpenAoIPWdm.cat','OpenAoIPWdmSetup.exe','wdm_runtime_tests.exe','README.md','WINDOWS-AUDIO.md','LICENSE','validation.json')
    Copy-Item -LiteralPath (Join-Path $taskBuild 'wdm-development/OpenAoIPWdm.sys') -Destination $taskPackage -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OpenAoIPWdm.inf') -Destination $taskPackage -Force
    foreach ($taskBinary in @('OpenAoIPWdmSetup.exe','wdm_runtime_tests.exe')) { Copy-Item -LiteralPath (Join-Path $taskBuild "$Configuration/$taskBinary") -Destination $taskPackage -Force }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $taskPackage -Force
    foreach ($taskDocument in @('WINDOWS-AUDIO.md','LICENSE')) { Copy-Item -LiteralPath (Join-Path $taskProject $taskDocument) -Destination $taskPackage -Force }
    $taskInf = Join-Path $taskPackage 'OpenAoIPWdm.inf'
    & (Join-Path $taskWdk 'tools/10.0.26100.0/x64/infverif.exe') /w $taskInf
    if ($LASTEXITCODE -ne 0) { throw 'INF validation failed' }
    $taskCatalog = Join-Path $taskPackage 'OpenAoIPWdm.cat'
    if (Test-Path -LiteralPath $taskCatalog) { Remove-Item -LiteralPath $taskCatalog }
    & (Join-Path $taskWdk 'bin/10.0.26100.0/x86/Inf2Cat.exe') "/driver:$taskPackage" /os:10_GE_X64
    if ($LASTEXITCODE -ne 0) { throw 'Catalog generation failed' }
    $taskValidation = @{ version='0.5.0';driverProtocol=2;stereoPairsPerDirection=16;sampleRate=48000;infVerif='passed';inf2Cat='passed';signed=$false;kernelRuntime='not-validated';repository='https://github.com/Block361/AudioTools';sha256=@{} }
    foreach ($taskName in @('OpenAoIPWdm.sys','OpenAoIPWdm.inf','OpenAoIPWdm.cat')) { $taskValidation.sha256[$taskName]=(Get-FileHash -LiteralPath (Join-Path $taskPackage $taskName) -Algorithm SHA256).Hash }
    $taskValidation | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskPackage 'validation.json') -Encoding UTF8
    $taskOutput = Join-Path $taskBuild 'packages/OpenAoIP-Windows-Audio-0.5.0-UNSIGNED-development-x64.zip'
    New-Item -ItemType Directory -Force -Path (Split-Path $taskOutput) | Out-Null
    $taskInputs = @($taskFiles | ForEach-Object { Join-Path $taskPackage $_ })
    Compress-Archive -LiteralPath $taskInputs -DestinationPath $taskOutput -Force
    Write-Output "Unsigned development package: $taskOutput"
    Write-Output 'No certificate was created/trusted; no driver was installed; no Windows boot policy was changed.'
} finally { Pop-Location }
