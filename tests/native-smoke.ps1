param([Parameter(Mandatory=$true)][string]$BinaryDirectory)
$ErrorActionPreference = 'Stop'
$BinaryDirectory = (Resolve-Path -LiteralPath $BinaryDirectory).Path
$taskRoot = Join-Path ([IO.Path]::GetTempPath()) ('OpenAoIP-native-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskRoot | Out-Null
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class NativeAoipTest {
  public delegate bool EnumProc(IntPtr window,IntPtr parameter);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr parameter);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window,StringBuilder name,int count);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr wp,IntPtr lp);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr window,uint message,IntPtr wp,IntPtr lp,uint flags,uint timeout,out IntPtr result);
  public static IntPtr Find(int pid,string className) {
    IntPtr result=IntPtr.Zero;
    EnumWindows((w,p)=>{uint id;GetWindowThreadProcessId(w,out id);var text=new StringBuilder(128);GetClassName(w,text,128);if(id==pid&&text.ToString()==className){result=w;return false;}return true;},IntPtr.Zero);
    return result;
  }
  public static void Message(IntPtr window,uint message,int wp,int lp) {
    IntPtr result;if(SendMessageTimeout(window,message,(IntPtr)wp,(IntPtr)lp,2,5000,out result)==IntPtr.Zero)throw new Exception("Native UI message timed out");
  }
  public static int Selected(IntPtr window,int id) { IntPtr result;if(SendMessageTimeout(GetDlgItem(window,id),0x147,IntPtr.Zero,IntPtr.Zero,2,5000,out result)==IntPtr.Zero)throw new Exception("Combo read timed out");return result.ToInt32(); }
  public static void Click(IntPtr window,int x,int y) {
    double scale=GetDpiForWindow(window)/96.0;
    int point=(((int)(y*scale))<<16)|((int)(x*scale)&65535);
    Message(window,0x201,1,point);Message(window,0x202,0,point);
  }
}
'@
function Wait-Condition([scriptblock]$Condition,[string]$Label) {
    $taskDeadline = [datetime]::UtcNow.AddSeconds(15)
    do { if (& $Condition) { return }; Start-Sleep -Milliseconds 100 } while ([datetime]::UtcNow -lt $taskDeadline)
    throw "Timed out: $Label"
}
$taskController = $null; $taskSoundcard = $null; $taskBridge = $null
$taskEndpoint = Join-Path $taskRoot 'engine.json'
try {
    # A loopback-only ephemeral TCP port avoids interfering with an existing controller.
    $taskProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0)
    $taskProbe.Start(); $taskPort = $taskProbe.LocalEndpoint.Port; $taskProbe.Stop()
    $taskController = Start-Process -FilePath (Join-Path $BinaryDirectory 'OpenAoIP Controller.exe') -ArgumentList @('--port',"$taskPort",'--data-dir',('"' + (Join-Path $taskRoot 'controller') + '"')) -WindowStyle Hidden -PassThru
    $taskUrl = "http://127.0.0.1:$taskPort"
    Wait-Condition { try { (Invoke-RestMethod "$taskUrl/api/state" -TimeoutSec 1).version -eq '0.6.1' } catch { $false } } 'native controller HTTP startup'
    $taskVscFolder = Join-Path $taskRoot 'soundcard'; New-Item -ItemType Directory -Path $taskVscFolder | Out-Null
    @{id='native-ui-test';name='Native UI Test';controller=$taskUrl;interface='127.0.0.1';txChannels=2;rxChannels=2;sampleRate=48000;latencyMs=60;backend='asio'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskVscFolder 'settings.json') -Encoding UTF8
    $taskSoundcard = Start-Process -FilePath (Join-Path $BinaryDirectory 'OpenAoIP Virtual Soundcard.exe') -ArgumentList @('--data-dir',('"' + $taskVscFolder + '"'),'--engine-endpoint',('"' + $taskEndpoint + '"')) -WindowStyle Hidden -PassThru
    Wait-Condition { [NativeAoipTest]::Find($taskSoundcard.Id,'OpenAoIPVirtualSoundcardNative') -ne [IntPtr]::Zero } 'native soundcard window'
    $taskVscWindow = [NativeAoipTest]::Find($taskSoundcard.Id,'OpenAoIPVirtualSoundcardNative')
    $taskControllerWindow = [NativeAoipTest]::Find($taskController.Id,'OpenAoIPControllerNative')
    if([NativeAoipTest]::Selected($taskVscWindow,108) -ne 0 -or [NativeAoipTest]::Selected($taskVscWindow,103) -ne 1){throw 'Native combo selections were not populated'}
    [NativeAoipTest]::Message($taskVscWindow,0x111,109,0) # Start button, no ASIO registration.
    Wait-Condition { $taskState = Invoke-RestMethod "$taskUrl/api/state"; @($taskState.devices | Where-Object id -eq 'native-ui-test').Count -eq 1 } 'soundcard registration from Start button'
    [NativeAoipTest]::Message($taskControllerWindow,0x113,1,0) # Refresh matrix.
    [NativeAoipTest]::Click($taskControllerWindow,325,348)
    Wait-Condition { (Invoke-RestMethod "$taskUrl/api/state").routes.Count -eq 1 } 'individual channel matrix click'
    $taskRoute = (Invoke-RestMethod "$taskUrl/api/state").routes[0]
    if ($taskRoute.senderChannel -ne 1 -or $taskRoute.receiverChannel -ne 1) { throw 'Native matrix selected the wrong channel' }
    $taskBridgeFolder=Join-Path $taskRoot 'bridge'; New-Item -ItemType Directory -Path $taskBridgeFolder | Out-Null
    @{id='native-bridge-test';name='Native Bridge Test';controller=$taskUrl;interface='127.0.0.1';txChannels=2;rxChannels=2;sampleRate=48000;endpoints=@(@{id='missing';name='Disconnected test input';device='id:missing-test-endpoint';mode='capture';patches=@(@{local=1;network=1;gain=1.0})})} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskBridgeFolder 'settings.json') -Encoding UTF8
    $taskBridge=Start-Process -FilePath (Join-Path $BinaryDirectory 'OpenAoIP Audio Bridge.exe') -ArgumentList @('--data-dir',('"'+$taskBridgeFolder+'"'),'--engine-endpoint',('"'+$taskEndpoint+'"')) -WindowStyle Hidden -PassThru
    Wait-Condition { [NativeAoipTest]::Find($taskBridge.Id,'OpenAoIPAudioBridgeNative') -ne [IntPtr]::Zero } 'native Audio Bridge window'
    $taskBridgeWindow=[NativeAoipTest]::Find($taskBridge.Id,'OpenAoIPAudioBridgeNative')
    [void][NativeAoipTest]::PostMessage($taskBridgeWindow,0x111,[IntPtr]401,[IntPtr]::Zero)
    Wait-Condition { [NativeAoipTest]::Find($taskBridge.Id,'OpenAoIPBridgeDeviceEditor') -ne [IntPtr]::Zero } 'native local device editor'
    [NativeAoipTest]::Message([NativeAoipTest]::Find($taskBridge.Id,'OpenAoIPBridgeDeviceEditor'),0x111,308,0)
    [NativeAoipTest]::Message($taskBridgeWindow,0x111,109,0)
    Wait-Condition { @((Invoke-RestMethod "$taskUrl/api/state").devices | Where-Object id -eq 'native-bridge-test').Count -eq 1 } 'concurrent soundcard and Audio Bridge registration'
    [NativeAoipTest]::Message($taskBridgeWindow,0x111,109,0)
    [NativeAoipTest]::Message($taskBridgeWindow,0x10,0,0)
    if(-not $taskBridge.WaitForExit(10000) -or $taskBridge.ExitCode -ne 0){ throw 'Native Audio Bridge teardown failed' }
    foreach ($taskX in @(180,312,444,576,48)) { [NativeAoipTest]::Click($taskControllerWindow,$taskX,157) }
    [NativeAoipTest]::Message($taskVscWindow,0x111,109,0) # Stop.
    Wait-Condition { -not (Invoke-RestMethod "$taskUrl/api/state").devices[0].online } 'soundcard goodbye on Stop'
    [NativeAoipTest]::Message([NativeAoipTest]::GetDlgItem($taskVscWindow,108),0x14e,1,0) # Choose Windows/WDM.
    [NativeAoipTest]::Message($taskVscWindow,0x111,65644,0) # CBN_SELCHANGE on API selector.
    if ([NativeAoipTest]::IsWindowEnabled([NativeAoipTest]::GetDlgItem($taskVscWindow,105))) { throw 'Windows/WDM mode did not lock the sample rate to 48 kHz' }
    [NativeAoipTest]::Message($taskVscWindow,0x10,0,0)
    [NativeAoipTest]::Message($taskControllerWindow,0x10,0,0)
    if (-not $taskSoundcard.WaitForExit(10000) -or -not $taskController.WaitForExit(10000)) { throw 'Native application teardown timed out' }
    if ($taskSoundcard.ExitCode -ne 0 -or $taskController.ExitCode -ne 0) { throw 'Native application exited with an error' }
    if (-not (Test-Path -LiteralPath (Join-Path $taskRoot 'controller/routes.json'))) { throw 'Native route was not persisted' }
    Write-Output 'PASS all three native applications, local-device editor, simultaneous Bridge/VSC registration, Start/Stop, per-channel matrix click, all controller tabs and clean teardown (no driver registration or physical recording).'
} finally {
    foreach ($taskProcess in @($taskBridge,$taskSoundcard,$taskController)) { if ($taskProcess -and -not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -Force } }
    if (Test-Path -LiteralPath $taskEndpoint) { $taskEngine = Get-Content -Raw -LiteralPath $taskEndpoint | ConvertFrom-Json; $taskEngineProcess=Get-Process -Id $taskEngine.pid -ErrorAction SilentlyContinue; if($taskEngineProcess -and $taskEngineProcess.Path -eq (Join-Path $BinaryDirectory 'OpenAoIP Engine.exe')) { Stop-Process -Id $taskEngineProcess.Id -Force } }
    # Test output remains in the system temporary folder for diagnostics; no recursive deletion.
}
