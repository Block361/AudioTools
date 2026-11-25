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
function Start-EndpointProcess([int]$Index, [string]$Arguments) {
    Start-Process -FilePath $taskExe -ArgumentList $Arguments -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $taskRoot "duplex-$taskTag-$Index.log") -RedirectStandardError (Join-Path $taskRoot "duplex-$taskTag-$Index.err.log")
}
function Wait-Condition([scriptblock]$Condition) {
    $taskEnd = [DateTime]::UtcNow.AddSeconds(12)
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 100 } while ([DateTime]::UtcNow -lt $taskEnd)
    throw 'Independent network-device process condition timed out.'
}
function Get-State { Invoke-RestMethod "$taskUrl/api/state" -TimeoutSec 2 }
function Connect-Route([string]$Source, [int]$Stream, [string]$Target, [int]$InputIndex) {
    $taskRoute = @{sender=$Source; stream="stream-$Stream"; receiver=$Target; input="input-$InputIndex"} | ConvertTo-Json -Compress
    Invoke-RestMethod "$taskUrl/api/routes" -Method Post -ContentType application/json -Body $taskRoute | Out-Null
}
function Check-Audio([string]$Device, [int]$InputIndex, [double]$Expected) {
    $taskFile = Join-Path $taskRoot "duplex-$taskTag-$Device-$InputIndex.wav"
    Invoke-WebRequest -UseBasicParsing "$taskUrl/api/audio/$Device/input-$InputIndex.wav" -OutFile $taskFile
    $taskBytes = [System.IO.File]::ReadAllBytes($taskFile)
    if ([System.Text.Encoding]::ASCII.GetString($taskBytes,0,4) -ne 'RIFF') { throw 'Invalid receiver WAV.' }
    $taskFrames = [Math]::Min(24000, ($taskBytes.Length - 44) / 4)
    $taskStart = ($taskBytes.Length - 44) / 4 - $taskFrames
    $taskCrossings = 0
    $taskPrevious = 0
    for ($taskFrame = $taskStart; $taskFrame -lt ($taskBytes.Length - 44) / 4; $taskFrame++) {
        $taskOffset = 44 + $taskFrame * 4
        $taskSample = [int]$taskBytes[$taskOffset] -bor ([int]$taskBytes[$taskOffset + 1] -shl 8)
        if ($taskSample -ge 32768) { $taskSample -= 65536 }
        if ($taskPrevious -lt 0 -and $taskSample -ge 0) { $taskCrossings++ }
        $taskPrevious = $taskSample
    }
    $taskFrequency = $taskCrossings * 48000.0 / $taskFrames
    if ([Math]::Abs($taskFrequency - $Expected) -gt 20) { throw "Unexpected received frequency at ${Device}/${InputIndex}: $taskFrequency (expected $Expected)." }
}
try {
    $taskProcesses += Start-EndpointProcess 0 "controller --port $taskPort --state duplex-$taskTag.json --duration 90"
    for ($taskIndex = 1; $taskIndex -le 3; $taskIndex++) {
        $taskConfig = @{id="peer-$taskIndex"; name="Network device $taskIndex"; controller=$taskUrl; host='127.0.0.1'; streams=2; inputs=2; tone=(220 + 220 * $taskIndex); playback='off'; port=0; monitorPort=0}
        $taskConfigName = "duplex-$taskTag-peer-$taskIndex.json"
        [System.IO.File]::WriteAllText((Join-Path $taskRoot $taskConfigName), ($taskConfig | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
        $taskProcesses += Start-EndpointProcess $taskIndex "device --config $taskConfigName --duration 90"
    }
    Wait-Condition { try { $taskState = Get-State; return $taskState.devices.Count -eq 3 } catch { return $false } }
    $taskState = Get-State
    foreach ($taskDevice in $taskState.devices) {
        if ($taskDevice.role -ne 'device' -or $taskDevice.streams.Count -ne 2 -or $taskDevice.inputs.Count -ne 2) { throw 'Duplex registration is incomplete.' }
    }
    Connect-Route peer-1 1 peer-2 1
    Connect-Route peer-2 1 peer-1 1
    Connect-Route peer-1 2 peer-3 1
    Connect-Route peer-3 1 peer-1 2
    Connect-Route peer-2 2 peer-3 2
    Connect-Route peer-1 1 peer-2 2
    Wait-Condition { $taskState = Get-State; return @($taskState.devices.inputs | Where-Object { $_.stats.capturedSeconds -gt 0.6 }).Count -eq 6 }
    Check-Audio peer-1 1 660
    Check-Audio peer-1 2 880
    Check-Audio peer-2 1 440
    Check-Audio peer-2 2 440
    Check-Audio peer-3 1 550
    Check-Audio peer-3 2 770
    # Abrupt termination exercises heartbeat timeout rather than graceful goodbye.
    Stop-Process -Id $taskProcesses[1].Id
    Wait-Condition { $taskPeer = (Get-State).devices | Where-Object id -eq 'peer-1'; return -not $taskPeer.online }
    $taskProcesses += Start-EndpointProcess 4 "device --config duplex-$taskTag-peer-1.json --duration 90"
    Wait-Condition { $taskPeer = (Get-State).devices | Where-Object id -eq 'peer-1'; return $taskPeer.online -and @($taskPeer.inputs | Where-Object { $_.stats.capturedSeconds -gt 0.6 }).Count -eq 2 }
    Check-Audio peer-1 1 660
    Write-Host 'PASS 3 independent duplex device processes; 6 simultaneous routes and measured audio frequencies; fan-out; crash timeout and automatic reconnection.'
} finally {
    foreach ($taskProcess in $taskProcesses) {
        if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -ErrorAction SilentlyContinue }
        $taskProcess.Dispose()
    }
}
