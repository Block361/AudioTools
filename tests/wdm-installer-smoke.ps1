param([Parameter(Mandatory=$true)][string]$BinaryDirectory,[Parameter(Mandatory=$true)][string]$Inf)
$ErrorActionPreference = 'Stop'
$taskFolder = Join-Path ([IO.Path]::GetTempPath()) ('OpenAoIP-signature-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskFolder | Out-Null
Copy-Item -LiteralPath $Inf -Destination (Join-Path $taskFolder 'OpenAoIPWdm.inf')
[IO.File]::WriteAllText((Join-Path $taskFolder 'OpenAoIPWdm.cat'),'invalid unsigned catalog')
[IO.File]::WriteAllText((Join-Path $taskFolder 'OpenAoIPWdm.sys'),'not a signed driver')
$taskReport = Join-Path $taskFolder 'report.json'
$taskProcess = Start-Process -FilePath (Join-Path $BinaryDirectory 'OpenAoIPWdmSetup.exe') -ArgumentList @('--check',('"'+(Join-Path $taskFolder 'OpenAoIPWdm.inf')+'"'),'--report',('"'+$taskReport+'"')) -WindowStyle Hidden -PassThru
if (-not $taskProcess.WaitForExit(10000)) { Stop-Process -Id $taskProcess.Id -Force; throw 'WDM signature check timed out' }
if ($taskProcess.ExitCode -ne 1) { throw 'Unsigned driver package was not rejected' }
$taskResult = Get-Content -LiteralPath $taskReport -Raw | ConvertFrom-Json
if ($taskResult.trusted -or $taskResult.installed -or -not $taskResult.error) { throw 'Invalid signature report' }
Write-Output 'PASS unsigned/tampered WDM package rejected in read-only check mode; no driver installation or security-policy changes.'
