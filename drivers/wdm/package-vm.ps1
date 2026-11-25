param([string]$BuildDirectory='build/desktop',[string]$Configuration='Release',[string]$WdkRoot='third_party/wdk/c')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
$taskProject=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Push-Location $taskProject
try {
    $taskBuild=(Resolve-Path -LiteralPath $BuildDirectory).Path
    $taskWdk=(Resolve-Path -LiteralPath $WdkRoot).Path
    $taskSignTool=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin/10.0.26100.0/x64/signtool.exe'
    if (-not (Test-Path -LiteralPath $taskSignTool)) { throw 'SignTool aus dem Windows SDK 26100 fehlt.' }
    $taskPackage=Join-Path $taskBuild 'wdm-package-vm-test'
    $taskPrivate=Join-Path $taskBuild 'private-vm-signing'
    New-Item -ItemType Directory -Force -Path $taskPackage,$taskPrivate | Out-Null
    # The development key stays local; never publish it or import it into a certificate store.
    $taskAcl=Get-Acl -LiteralPath $taskPrivate
    $taskAcl.SetAccessRuleProtection($true,$false)
    $taskPrincipals=@([Security.Principal.WindowsIdentity]::GetCurrent().User,[Security.Principal.NTAccount]::new((Get-Acl -LiteralPath $taskProject).Owner),[Security.Principal.SecurityIdentifier]::new('S-1-5-18'))
    foreach ($taskPrincipal in $taskPrincipals) {
        $taskRule=[Security.AccessControl.FileSystemAccessRule]::new($taskPrincipal,'FullControl','ContainerInherit,ObjectInherit','None','Allow')
        $taskAcl.SetAccessRule($taskRule)
    }
    [IO.Directory]::SetAccessControl($taskPrivate,$taskAcl)
    $taskPfx=Join-Path $taskPrivate 'OpenAoIP-VM-Test.pfx'
    $taskPasswordFile=Join-Path $taskPrivate 'OpenAoIP-VM-Test.password'
    $taskHasPassword=Test-Path -LiteralPath $taskPasswordFile
    $taskPfxPassword=if ($taskHasPassword) { [IO.File]::ReadAllText($taskPasswordFile) } else { [guid]::NewGuid().ToString('N') }
    if (Test-Path -LiteralPath $taskPfx) {
        $taskOldPassword=if ($taskHasPassword) { $taskPfxPassword } else { '' }
        $taskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($taskPfx,$taskOldPassword,([Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet -bor [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable))
        if ($taskCertificate.NotAfter -lt [datetime]::Now.AddDays(7)) { throw 'Lokales VM-Testzertifikat laeuft ab. Schluessel bewusst erneuern und Test-VM neu vorbereiten.' }
        if (-not $taskHasPassword) { [IO.File]::WriteAllBytes($taskPfx,$taskCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx,$taskPfxPassword)) }
    } else {
        $taskRsa=[Security.Cryptography.RSACng]::new(2048)
        try {
            $taskRequest=[Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=OpenAoIP VirtualBox Development Test',$taskRsa,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
            $taskRequest.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false,$false,0,$true))
            $taskRequest.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new([Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,$true))
            $taskEku=[Security.Cryptography.OidCollection]::new();[void]$taskEku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
            $taskRequest.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($taskEku,$false))
            $taskCertificate=$taskRequest.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1),[DateTimeOffset]::UtcNow.AddDays(90))
            [IO.File]::WriteAllBytes($taskPfx,$taskCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx,$taskPfxPassword))
        } finally { $taskRsa.Dispose() }
    }
    [IO.File]::WriteAllText($taskPasswordFile,$taskPfxPassword)
    function Assert-NoHostTrust {
        foreach ($taskLocation in @('CurrentUser','LocalMachine')) {
            foreach ($taskName in @('Root','TrustedPublisher')) {
                $taskStore=[Security.Cryptography.X509Certificates.X509Store]::new($taskName,$taskLocation)
                try {
                    $taskStore.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
                    if ($taskStore.Certificates.Find([Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,$taskCertificate.Thumbprint,$false).Count) { throw 'Das VM-Testzertifikat ist auf dem Build-PC bereits vertraut. Dieses Paket wird ohne Aenderungen an Host-Vertrauensspeichern gebaut.' }
                } finally { $taskStore.Close() }
            }
        }
    }
    Assert-NoHostTrust
    [IO.File]::WriteAllBytes((Join-Path $taskPackage 'OpenAoIP-VM-Test.cer'),$taskCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    Copy-Item -LiteralPath (Join-Path $taskBuild 'wdm-development/OpenAoIPWdm.sys') -Destination $taskPackage -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OpenAoIPWdm.inf') -Destination $taskPackage -Force
    foreach ($taskName in @('OpenAoIPWdmSetup.exe','wdm_runtime_tests.exe','OpenAoIPControllerSetup.exe','OpenAoIPVirtualSoundcardSetup.exe')) { Copy-Item -LiteralPath (Join-Path $taskBuild "$Configuration/$taskName") -Destination $taskPackage -Force }
    foreach ($taskName in @('LICENSE','WINDOWS-AUDIO.md','WINDOWS-START.md')) { Copy-Item -LiteralPath (Join-Path $taskProject $taskName) -Destination $taskPackage -Force }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'VM-TEST.md') -Destination $taskPackage -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vm-test.ps1') -Destination (Join-Path $taskPackage 'VM-TEST.ps1') -Force
    $taskDriver=Join-Path $taskPackage 'OpenAoIPWdm.sys'
    & $taskSignTool sign /fd SHA256 /ph /f $taskPfx /p $taskPfxPassword $taskDriver
    if ($LASTEXITCODE -ne 0) { throw 'Testsignierung der SYS-Datei fehlgeschlagen.' }
    & (Join-Path $taskWdk 'tools/10.0.26100.0/x64/infverif.exe') /w (Join-Path $taskPackage 'OpenAoIPWdm.inf')
    if ($LASTEXITCODE -ne 0) { throw 'INF-Pruefung fehlgeschlagen.' }
    $taskCatalog=Join-Path $taskPackage 'OpenAoIPWdm.cat'
    if (Test-Path -LiteralPath $taskCatalog) { Remove-Item -LiteralPath $taskCatalog }
    & (Join-Path $taskWdk 'bin/10.0.26100.0/x86/Inf2Cat.exe') "/driver:$taskPackage" /os:10_GE_X64
    if ($LASTEXITCODE -ne 0) { throw 'Katalogerstellung fehlgeschlagen.' }
    & $taskSignTool sign /fd SHA256 /f $taskPfx /p $taskPfxPassword $taskCatalog
    if ($LASTEXITCODE -ne 0) { throw 'Testsignierung des Katalogs fehlgeschlagen.' }
    # Check signatures cryptographically, without changing the host trust stores.
    function Assert-Cms([byte[]]$Bytes) {
        $taskCms=[Security.Cryptography.Pkcs.SignedCms]::new();$taskCms.Decode($Bytes);$taskCms.CheckSignature($true)
        if ($taskCms.SignerInfos.Count -ne 1 -or $taskCms.SignerInfos[0].Certificate.Thumbprint -ne $taskCertificate.Thumbprint) { throw 'Signatur gehoert nicht zum VM-Testzertifikat.' }
        return ,$taskCms.ContentInfo.Content
    }
    function Assert-EmbeddedDriver([byte[]]$Bytes) {
        $taskPe=[BitConverter]::ToInt32($Bytes,60);$taskOptional=$taskPe+24
        if ([BitConverter]::ToUInt16($Bytes,$taskOptional) -ne 0x20b) { throw 'Kein x64-PE-Binaerformat.' }
        $taskChecksum=$taskOptional+64;$taskEntry=$taskOptional+112+4*8
        $taskOffset=[BitConverter]::ToInt32($Bytes,$taskEntry);$taskLength=[BitConverter]::ToInt32($Bytes,$taskEntry+4)
        if ($taskOffset -lt $taskEntry+8 -or $taskLength -lt 8 -or $taskOffset+$taskLength -gt $Bytes.Length) { throw 'PE-Signaturtabelle ungueltig.' }
        $taskDerSize=[BitConverter]::ToInt32($Bytes,$taskOffset)-8
        $taskCmsContent=Assert-Cms ([byte[]]$Bytes[($taskOffset+8)..($taskOffset+7+$taskDerSize)])
        if ($taskCmsContent[-34] -ne 4 -or $taskCmsContent[-33] -ne 32) { throw 'SHA256-Indirektdaten nicht erkannt.' }
        $taskStream=[IO.MemoryStream]::new()
        try {
            $taskStream.Write($Bytes,0,$taskChecksum)
            $taskStream.Write($Bytes,$taskChecksum+4,$taskEntry-$taskChecksum-4)
            $taskStream.Write($Bytes,$taskEntry+8,$taskOffset-$taskEntry-8)
            if ($taskOffset+$taskLength -lt $Bytes.Length) { $taskStream.Write($Bytes,$taskOffset+$taskLength,$Bytes.Length-$taskOffset-$taskLength) }
            $taskHasher=[Security.Cryptography.SHA256]::Create()
            try { $taskDigest=$taskHasher.ComputeHash($taskStream.ToArray()) } finally { $taskHasher.Dispose() }
        } finally { $taskStream.Dispose() }
        if ([BitConverter]::ToString($taskDigest) -ne [BitConverter]::ToString([byte[]]$taskCmsContent[($taskCmsContent.Length-32)..($taskCmsContent.Length-1)])) { throw 'PE-Dateihash passt nicht zur eingebetteten Signatur.' }
    }
    Assert-EmbeddedDriver ([IO.File]::ReadAllBytes($taskDriver))
    $null=Assert-Cms ([IO.File]::ReadAllBytes($taskCatalog))
    # A changed driver must fail even though its signature block is still present.
    $taskCorrupt=[IO.File]::ReadAllBytes($taskDriver)
    $taskPe=[BitConverter]::ToInt32($taskCorrupt,60);$taskSections=$taskPe+24+[BitConverter]::ToUInt16($taskCorrupt,$taskPe+20)
    $taskRaw=[BitConverter]::ToInt32($taskCorrupt,$taskSections+20);$taskCorrupt[$taskRaw]=$taskCorrupt[$taskRaw] -bxor 1
    $taskRejected=$false;try { Assert-EmbeddedDriver $taskCorrupt } catch { $taskRejected=$true }
    if (-not $taskRejected) { throw 'Negativtest: Veraenderter Treiber wurde nicht erkannt.' }
    Assert-NoHostTrust
    $taskFiles=@('OpenAoIPWdm.sys','OpenAoIPWdm.inf','OpenAoIPWdm.cat','OpenAoIPWdmSetup.exe','wdm_runtime_tests.exe','OpenAoIPControllerSetup.exe','OpenAoIPVirtualSoundcardSetup.exe','OpenAoIP-VM-Test.cer','VM-TEST.ps1','VM-TEST.md','WINDOWS-AUDIO.md','WINDOWS-START.md','LICENSE')
    $taskManifest=@{version='0.5.0';signature='self-signed-development-VM-only';certificateThumbprint=$taskCertificate.Thumbprint;certificateExpires=$taskCertificate.NotAfter.ToUniversalTime().ToString('o');embeddedDriverSignatureChecked=$true;catalogSignatureChecked=$true;tamperNegativeTestPassed=$true;trustedOnBuildHost=$false;kernelRuntime='not-tested';sha256=@{}}
    foreach ($taskName in $taskFiles) { $taskManifest.sha256[$taskName]=(Get-FileHash -LiteralPath (Join-Path $taskPackage $taskName) -Algorithm SHA256).Hash }
    $taskManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskPackage 'test-signing.json') -Encoding UTF8
    $taskZip=Join-Path $taskBuild 'packages/OpenAoIP-Windows-Audio-0.5.0-TESTSIGNED-VirtualBox-x64.zip'
    $taskInputs=@(($taskFiles+@('test-signing.json')) | ForEach-Object { Join-Path $taskPackage $_ })
    Compress-Archive -LiteralPath $taskInputs -DestinationPath $taskZip -Force
    Write-Output "VM-Paket: $taskZip"
    Write-Output "Oeffentlicher Zertifikatsfingerabdruck: $($taskCertificate.Thumbprint)"
    Write-Output 'Signaturen und Manipulations-Negativtest geprueft. Der private Schluessel bleibt lokal. Auf diesem PC wurden keine Zertifikate vertraut, keine Treiber installiert und keine Bootoptionen geaendert.'
} finally { Pop-Location }
