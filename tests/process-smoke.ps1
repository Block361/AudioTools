param([Parameter(Mandatory = $true)][string]$Executable)
$ErrorActionPreference = 'Stop'
$taskExe = (Resolve-Path -LiteralPath $Executable).Path
$taskRoot = Split-Path $taskExe
$taskTag = [guid]::NewGuid().ToString('N')
$taskProcesses = @()
$taskListener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$taskListener.Start()
$taskPort = $taskListener.LocalEndpoint.Port
$taskListener.Stop()
$taskUrl = "http://127.0.0.1:$taskPort"
try {
    $taskArguments = @(
        "controller --port $taskPort --state process-$taskTag.json --duration 25",
        "sender --id process-tx --streams 2 --controller $taskUrl --duration 25",
        "receiver --id process-rx --inputs 2 --port 0 --host 127.0.0.1 --controller $taskUrl --duration 25"
    )
    for ($taskIndex = 0; $taskIndex -lt $taskArguments.Count; $taskIndex++) {
        $taskProcesses += Start-Process -FilePath $taskExe -ArgumentList $taskArguments[$taskIndex] -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskRoot "process-$taskTag-$taskIndex.log") -RedirectStandardError (Join-Path $taskRoot "process-$taskTag-$taskIndex.err.log")
    }
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        try { $taskState = Invoke-RestMethod "$taskUrl/api/state" -TimeoutSec 2 } catch { $taskState = $null }
    } until (($taskState -and $taskState.devices.Count -eq 2) -or [DateTime]::UtcNow -gt $taskDeadline)
    if (-not $taskState -or $taskState.devices.Count -ne 2) { throw 'Independent device processes did not register.' }
    foreach ($taskChannel in 1,2) {
        $taskRoute = @{sender='process-tx';stream="stream-$taskChannel";receiver='process-rx';input="input-$taskChannel"} | ConvertTo-Json -Compress
        Invoke-RestMethod "$taskUrl/api/routes" -Method Post -ContentType application/json -Body $taskRoute | Out-Null
    }
    $taskDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $taskState = Invoke-RestMethod "$taskUrl/api/state" -TimeoutSec 2
        $taskRx = $taskState.devices | Where-Object id -eq 'process-rx'
        $taskReady = @($taskRx.inputs | Where-Object { $_.stats.capturedSeconds -gt 0.5 -and $_.stats.rms -gt 0.1 }).Count
    } until ($taskReady -eq 2 -or [DateTime]::UtcNow -gt $taskDeadline)
    if ($taskReady -ne 2) { throw 'Both independent-process audio streams did not arrive.' }
    $taskCapture = Join-Path $taskRoot "process-$taskTag.wav"
    Invoke-WebRequest -UseBasicParsing "$taskUrl/api/audio/process-rx/input-1.wav" -OutFile $taskCapture
    $taskBytes = [System.IO.File]::ReadAllBytes($taskCapture)
    if ([System.Text.Encoding]::ASCII.GetString($taskBytes,0,4) -ne 'RIFF' -or $taskBytes.Length -le 96044) { throw 'Receiver WAV capture is invalid or empty.' }
    Write-Host 'PASS independent controller, sender and receiver processes; 2 simultaneous UDP audio streams; WAV capture.'
} finally {
    foreach ($taskProcess in $taskProcesses) {
        if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
        $taskProcess.Dispose()
    }
}
