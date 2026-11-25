param([ValidateSet('Status','Prepare','Install','Test','Remove')][string]$Action='Status')
$ErrorActionPreference='Stop'
# Explicit guest-only actions. This file is copied into the VM test package.
$taskSystem=Get-CimInstance Win32_ComputerSystem
if ($taskSystem.Model -notmatch '^VirtualBox') { throw 'Abbruch: Dieses Skript darf nur innerhalb der Windows-VirtualBox-VM ausgefuehrt werden.' }
$taskOs=Get-CimInstance Win32_OperatingSystem
if ([int]$taskOs.BuildNumber -lt 26100 -or -not [Environment]::Is64BitOperatingSystem) { throw 'Windows 11 x64 Build 26100 (24H2) oder neuer ist erforderlich.' }
$taskManifest=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'test-signing.json') -Raw | ConvertFrom-Json
foreach ($taskEntry in $taskManifest.sha256.PSObject.Properties) {
    if ([IO.Path]::GetFileName($taskEntry.Name) -ne $taskEntry.Name) { throw 'Ungueltiger Paketpfad' }
    if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $taskEntry.Name) -Algorithm SHA256).Hash -ne $taskEntry.Value) { throw "Paketdatei veraendert: $($taskEntry.Name)" }
}
$taskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $PSScriptRoot 'OpenAoIP-VM-Test.cer'))
if ($taskCertificate.Thumbprint -ne $taskManifest.certificateThumbprint -or $taskCertificate.HasPrivateKey) { throw 'Testzertifikat passt nicht zum Paket' }
$taskIdentity=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
$taskAdmin=$taskIdentity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$taskStateFile=Join-Path $PSScriptRoot 'vm-test-state.json'
$taskBoot=$taskOs.LastBootUpTime.ToUniversalTime().ToString('o')
Write-Host "VirtualBox-VM erkannt. Windows Build $($taskOs.BuildNumber). Aktion: $Action"
Write-Host "Testzertifikat: $($taskCertificate.Thumbprint)"
if ($Action -eq 'Status') {
    Write-Host "Administrator: $taskAdmin. Paketpruefung bestanden. Noch keine Einstellungen geaendert."
    exit 0
}
if (-not $taskAdmin) { throw 'Windows PowerShell innerhalb der VM als Administrator oeffnen.' }
function Read-TestMode {
    $taskBcd=& "$env:SystemRoot/System32/bcdedit.exe" /enum '{current}' 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Bootkonfiguration konnte nicht gelesen werden' }
    $taskLine=@($taskBcd | Where-Object { "$_" -match '^\s*testsigning\s+' })
    if (-not $taskLine.Count) { return $false }
    if ("$($taskLine[0])" -match '\s+(Yes|Ja|On|1)\s*$') { return $true }
    if ("$($taskLine[0])" -match '\s+(No|Nein|Off|0)\s*$') { return $false }
    throw 'Status von TESTSIGNING unbekannt. Bitte englisches/deutsches Windows verwenden oder Bootkonfiguration manuell pruefen.'
}
function Open-TestStore([string]$Name) {
    $taskStore=[Security.Cryptography.X509Certificates.X509Store]::new($Name,[Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)
    $taskStore.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    return $taskStore
}
function Run-Tool([string]$Argument) {
    $taskProcess=Start-Process -FilePath (Join-Path $PSScriptRoot 'OpenAoIPWdmSetup.exe') -ArgumentList $Argument -WindowStyle Hidden -PassThru
    $taskProcess.WaitForExit()
    if ($taskProcess.ExitCode -ne 0) { throw "Installationshelfer meldet Fehler $($taskProcess.ExitCode)." }
}
if ($Action -eq 'Prepare') {
    $taskSecure=Get-ItemProperty 'HKLM:/SYSTEM/CurrentControlSet/Control/SecureBoot/State' -ErrorAction SilentlyContinue
    if ($taskSecure -and $taskSecure.UEFISecureBootEnabled -eq 1) { throw 'Secure Boot ist in dieser VM aktiv. Vor einem Treibertest nur die VM entsprechend konfigurieren; am Host nichts aendern. Danach Prepare erneut starten.' }
    if (Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue) {
        $taskVolume=Get-BitLockerVolume -MountPoint $env:SystemDrive
        if ($taskVolume.ProtectionStatus -eq 'On') { throw 'BitLocker-Schutz ist in der VM aktiv. Wiederherstellungsschluessel sichern und die Bootaenderung dort erst bewusst vorbereiten. Das Skript aendert BitLocker nicht.' }
    }
    if (Test-Path -LiteralPath $taskStateFile) { throw 'VM-Test wurde bereits vorbereitet. Jetzt neu starten und Install verwenden, oder zuerst Remove ausfuehren.' }
    $taskState=@{certificateThumbprint=$taskCertificate.Thumbprint;preparedBoot=$taskBoot;previousTestMode=(Read-TestMode);addedRoot=$false;addedPublisher=$false}
    foreach ($taskName in @('Root','TrustedPublisher')) {
        $taskStore=Open-TestStore $taskName
        try {
            $taskExists=$taskStore.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$taskCertificate.Thumbprint,$false).Count -gt 0
            if ($taskName -eq 'Root') { $taskState.addedRoot=-not $taskExists } else { $taskState.addedPublisher=-not $taskExists }
        } finally { $taskStore.Close() }
    }
    $taskState | ConvertTo-Json | Set-Content -LiteralPath $taskStateFile -Encoding UTF8
    foreach ($taskName in @('Root','TrustedPublisher')) { $taskStore=Open-TestStore $taskName;try { $taskStore.Add($taskCertificate) } finally { $taskStore.Close() } }
    & "$env:SystemRoot/System32/bcdedit.exe" /set '{current}' TESTSIGNING ON
    if ($LASTEXITCODE -ne 0) { throw 'TESTSIGNING konnte nicht aktiviert werden. Mit Remove koennen die hinzugefuegten Zertifikate wieder entfernt werden.' }
    Write-Host 'Nur in dieser VM wurden das Testzertifikat vertraut und TESTSIGNING aktiviert. VM jetzt neu starten; danach Aktion Install.'
    exit 0
}
if ($Action -eq 'Install') {
    if (-not (Test-Path -LiteralPath $taskStateFile)) { throw 'Zuerst Prepare ausfuehren.' }
    $taskState=Get-Content -LiteralPath $taskStateFile -Raw | ConvertFrom-Json
    if ($taskState.preparedBoot -eq $taskBoot -or -not (Read-TestMode)) { throw 'Nach Prepare muss die VM neu gestartet werden; TESTSIGNING muss aktiv sein.' }
    Run-Tool ('--install "'+(Join-Path $PSScriptRoot 'OpenAoIPWdm.inf')+'"')
    Write-Host 'Installationshelfer beendet. Falls Windows einen Neustart verlangt hat, zuerst neu starten; danach Aktion Test.'
    exit 0
}
if ($Action -eq 'Test') {
    $taskLogs=Join-Path $PSScriptRoot 'test-logs';New-Item -ItemType Directory -Force -Path $taskLogs | Out-Null
    $taskOut=Join-Path $taskLogs 'runtime-stdout.txt';$taskErr=Join-Path $taskLogs 'runtime-stderr.txt'
    $taskProcess=Start-Process -FilePath (Join-Path $PSScriptRoot 'wdm_runtime_tests.exe') -WindowStyle Hidden -RedirectStandardOutput $taskOut -RedirectStandardError $taskErr -PassThru
    if (-not $taskProcess.WaitForExit(45000)) { Stop-Process -Id $taskProcess.Id -Force;throw "Audiotest hat nicht rechtzeitig geantwortet. Logs: $taskLogs" }
    Get-Content -LiteralPath $taskOut;Get-Content -LiteralPath $taskErr
    Get-CimInstance Win32_PnPEntity | Where-Object { $_.Service -eq 'OpenAoIPWdm' } | Select-Object Name,Status,ConfigManagerErrorCode,PNPDeviceID | Format-List | Out-File -LiteralPath (Join-Path $taskLogs 'device.txt')
    if ($taskProcess.ExitCode -eq 77) { throw 'Treiber ist nicht verfuegbar: Der echte Audiotest wurde NICHT bestanden, sondern uebersprungen.' }
    if ($taskProcess.ExitCode -ne 0) { throw "Audiotest fehlgeschlagen. Bitte test-logs und gegebenenfalls den Bluescreen-Stopcode fuer die Fehlersuche aufbewahren." }
    Write-Host 'Der echte Audiotest wurde bestanden. Das ersetzt noch keine Langzeit-/PnP-/Verifier-Tests.'
    exit 0
}
if ($Action -eq 'Remove') {
    Run-Tool '--remove'
    if (@(Get-CimInstance Win32_PnPEntity | Where-Object { $_.Service -eq 'OpenAoIPWdm' }).Count) { throw 'OpenAoIP-Geraet ist noch vorhanden. Entfernung bestaetigen, gegebenenfalls VM neu starten, dann Remove erneut ausfuehren.' }
    if (Test-Path -LiteralPath $taskStateFile) {
        $taskState=Get-Content -LiteralPath $taskStateFile -Raw | ConvertFrom-Json
        if ($taskState.certificateThumbprint -ne $taskCertificate.Thumbprint) { throw 'Rueckbauzustand gehoert zu einem anderen Testzertifikat.' }
        foreach ($taskName in @('Root','TrustedPublisher')) {
            $taskAdded=if ($taskName -eq 'Root') { $taskState.addedRoot } else { $taskState.addedPublisher }
            if ($taskAdded) { $taskStore=Open-TestStore $taskName;try { foreach ($taskFound in $taskStore.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$taskCertificate.Thumbprint,$false)) { $taskStore.Remove($taskFound) } } finally { $taskStore.Close() } }
        }
        if (-not $taskState.previousTestMode) { & "$env:SystemRoot/System32/bcdedit.exe" /set '{current}' TESTSIGNING OFF;if ($LASTEXITCODE -ne 0) { throw 'TESTSIGNING konnte nicht zurueckgesetzt werden.' } }
        Remove-Item -LiteralPath $taskStateFile
    }
    Write-Host 'OpenAoIP-Testeinrichtung entfernt. VM neu starten oder den sauberen Snapshot wiederherstellen.'
}
