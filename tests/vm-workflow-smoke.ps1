param([Parameter(Mandatory=$true)][string]$SourceDirectory,[string]$PackageDirectory='',[string]$Zip='')
$ErrorActionPreference='Stop'
foreach ($taskRelative in @('drivers/wdm/vm-test.ps1','drivers/wdm/package-vm.ps1')) {
    $taskTokens=$null;$taskErrors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceDirectory $taskRelative),[ref]$taskTokens,[ref]$taskErrors)
    if ($taskErrors.Count) { throw "PowerShell-Syntaxfehler: $($taskErrors | Out-String)" }
}
# Exercise every action with a simulated physical system: all must stop before any changes.
function Get-CimInstance { param([string]$ClassName) if ($ClassName -ne 'Win32_ComputerSystem') { throw 'Guard did not stop before further system queries' };[pscustomobject]@{Model='Physical-PC-guard-test'} }
foreach ($taskAction in @('Status','Prepare','Install','Test','Remove')) {
    $taskRejected=$false
    try { & (Join-Path $SourceDirectory 'drivers/wdm/vm-test.ps1') -Action $taskAction }
    catch { if ($_.Exception.Message -like '*nur innerhalb der Windows-VirtualBox-VM*') { $taskRejected=$true } else { throw } }
    if (-not $taskRejected) { throw "VM action did not reject a physical machine: $taskAction" }
}
if ($PackageDirectory) {
    $taskManifest=Get-Content -LiteralPath (Join-Path $PackageDirectory 'test-signing.json') -Raw | ConvertFrom-Json
    if (-not $taskManifest.embeddedDriverSignatureChecked -or -not $taskManifest.catalogSignatureChecked -or -not $taskManifest.tamperNegativeTestPassed -or $taskManifest.trustedOnBuildHost) { throw 'Signature validation metadata missing or incorrect' }
    foreach ($taskFile in $taskManifest.sha256.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath (Join-Path $PackageDirectory $taskFile.Name)).Hash -ne $taskFile.Value) { throw "VM payload hash differs: $($taskFile.Name)" }
    }
    $taskCert=[Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $PackageDirectory 'OpenAoIP-VM-Test.cer'))
    if ($taskCert.HasPrivateKey -or $taskCert.Thumbprint -ne $taskManifest.certificateThumbprint) { throw 'Public certificate mismatch or private key included' }
    foreach ($taskLocation in @('CurrentUser','LocalMachine')) { foreach ($taskName in @('Root','TrustedPublisher')) {
        $taskStore=[Security.Cryptography.X509Certificates.X509Store]::new($taskName,$taskLocation)
        try { $taskStore.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly);if ($taskStore.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$taskCert.Thumbprint,$false).Count) { throw 'VM certificate unexpectedly trusted on test host' } } finally { $taskStore.Close() }
    } }
}
if ($Zip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $taskArchive=[IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Zip).Path)
    try {
        if (@($taskArchive.Entries | Where-Object { $_.FullName -match '(?i)\.(pfx|p12|password)$|private-vm-signing' }).Count) { throw 'Private signing material found in VM ZIP' }
        foreach ($taskName in @('OpenAoIPWdm.sys','OpenAoIPWdm.cat','OpenAoIPWdm.inf','OpenAoIP-VM-Test.cer','VM-TEST.ps1','VM-TEST.md','wdm_runtime_tests.exe','test-signing.json')) { if (-not $taskArchive.GetEntry($taskName)) { throw "Missing VM ZIP file: $taskName" } }
    } finally { $taskArchive.Dispose() }
}
Write-Output 'PASS VM script syntax, physical-host rejection for all actions, and supplied VM package hashes/public-only certificate/absence of host trust.'
