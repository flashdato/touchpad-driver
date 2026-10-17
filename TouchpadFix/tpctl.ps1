<#
  tpctl.ps1 - install, check and tune the htixtp touchpad driver
              (Infinix INBook X3 XL422, Hantick 0911:5288 I2C touchpad)

  Open PowerShell as administrator in this folder, then:

    Set-ExecutionPolicy -Scope Process Bypass -Force
    .\tpctl.ps1 prepare        check Secure Boot / test signing / encryption, enable test signing
    .\tpctl.ps1 install        trust the test certificate and install the driver on the touchpad
    .\tpctl.ps1 status         what the driver sees right now, and what to do next
    .\tpctl.ps1 test           25-second input test (move the USB mouse first, then the touchpad)
    .\tpctl.ps1 mode auto|full|mouse      precision touchpad, or plain mouse (gestures off)
    .\tpctl.ps1 polling off|on|both       interrupt (default) or polling the chip
    .\tpctl.ps1 restart        restart the touchpad device (safe with this driver)
    .\tpctl.ps1 collect        save a full report to Desktop\TouchpadFix
    .\tpctl.ps1 uninstall      remove this driver; Windows goes back to its own
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('prepare', 'install', 'status', 'test', 'mode', 'polling', 'restart', 'collect', 'uninstall', 'help')]
    [string]$Command = 'help',

    [Parameter(Position = 1)]
    [string]$Value
)

$ErrorActionPreference = 'Continue'
$Here      = $PSScriptRoot
$InfPath   = Join-Path $Here 'driver\htixtp.inf'
$CerPath   = Join-Path $Here 'TouchpadFix.cer'
$OurIds    = @('ACPI\VEN_SYN&DEV_A360', 'ACPI\SYNA360', 'ACPI\VEN_SYNA&DEV_3602', 'ACPI\SYNA3602', 'ACPI\VEN_HTIX&DEV_5288', 'ACPI\HTIX5288')

# ----------------------------------------------------------------------------- helpers

function Write-Title([string]$Text) { Write-Host ''; Write-Host "=== $Text ===" -ForegroundColor Cyan }
function Write-Good([string]$Text)  { Write-Host "  $Text" -ForegroundColor Green }
function Write-Bad([string]$Text)   { Write-Host "  $Text" -ForegroundColor Red }
function Write-Note([string]$Text)  { Write-Host "  $Text" -ForegroundColor Yellow }
function Write-Line([string]$Text)  { Write-Host "  $Text" }

function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Assert-Admin {
    if (-not (Test-Admin)) {
        Write-Bad 'Run this from PowerShell opened as administrator (right-click PowerShell > Run as administrator).'
        exit 1
    }
    if (-not [Environment]::Is64BitProcess) {
        Write-Bad 'Run this from 64-bit PowerShell (the normal "Windows PowerShell", not the (x86) one).'
        exit 1
    }
}

function Get-DevProp([string]$Id, [string]$Key) {
    try { return (Get-PnpDeviceProperty -InstanceId $Id -KeyName $Key -ErrorAction Stop).Data } catch { return $null }
}

function Get-Touchpad {
    $all = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^ACPI\\(SYNA3602?|HTIX5288)\\' })
    $present = $all | Where-Object { $_.Present } | Select-Object -First 1
    if ($present) { return $present }
    return ($all | Select-Object -First 1)
}

function Get-ProblemCode($Dev) {
    try { return [int]$Dev.ConfigManagerErrorCode } catch { return -1 }
}

function Get-ParamsPath($Tp) {
    return "HKLM:\SYSTEM\CurrentControlSet\Enum\$($Tp.InstanceId)\Device Parameters"
}

function Get-Params($Tp) {
    return Get-ItemProperty -Path (Get-ParamsPath $Tp) -ErrorAction SilentlyContinue
}

function Test-OurDriver($Tp) {
    $filters = Get-DevProp $Tp.InstanceId 'DEVPKEY_Device_LowerFilters'
    $provider = Get-DevProp $Tp.InstanceId 'DEVPKEY_Device_DriverProvider'
    return (($filters -contains 'htixtp') -or ($provider -eq 'TouchpadFix'))
}

function Get-TestSigning {
    $text = (& bcdedit.exe /enum '{current}' 2>&1 | Out-String)
    if ($text -match '(?im)^\s*testsigning\s+(Yes|On)\b') { return $true }
    if ($text -match '(?i)access is denied|requested system device cannot be found') { return $null }
    return $false
}

function Get-SecureBoot {
    try { return [bool](Confirm-SecureBootUEFI -ErrorAction Stop) } catch { return $null }
}

function Get-EncryptionOn {
    $text = (& manage-bde.exe -status $env:SystemDrive 2>&1 | Out-String)
    if ($text -match '(?im)Protection Status:\s*Protection On') { return $true }
    if ($text -match '(?im)Protection Status:\s*Protection Off') { return $false }
    return $null
}

function Get-MemoryIntegrityRunning {
    try {
        $dg = Get-CimInstance -Namespace 'root\Microsoft\Windows\DeviceGuard' -ClassName Win32_DeviceGuard -ErrorAction Stop
        return (@($dg.SecurityServicesRunning) -contains 2)
    } catch { return $null }
}

function Restart-Touchpad($Tp) {
    Write-Line "Restarting $($Tp.InstanceId) ..."
    & pnputil.exe /restart-device "$($Tp.InstanceId)" | Out-Null
    Start-Sleep -Seconds 4
}

function Get-TouchpadChildren($Tp) {
    return @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'HID\*' } |
             Where-Object { (Get-DevProp $_.InstanceId 'DEVPKEY_Device_Parent') -eq $Tp.InstanceId })
}

# Re-enable touchpad collections that were disabled earlier (e.g. the Input Configuration device).
function Enable-TouchpadChildren($Tp) {
    foreach ($k in (Get-TouchpadChildren $Tp)) {
        if ($k.Present -and (Get-ProblemCode $k) -eq 22) {
            Write-Line "Re-enabling $($k.FriendlyName) [$($k.InstanceId)]"
            Enable-PnpDevice -InstanceId $k.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
        }
    }
}

function Set-DeviceParam($Tp, [string]$Name, [int]$Number) {
    $path = Get-ParamsPath $Tp
    if (-not (Test-Path $path)) { New-Item -Path $path -Force | Out-Null }
    Set-ItemProperty -Path $path -Name $Name -Value $Number -Type DWord
}

# ----------------------------------------------------------------------------- prepare

function Invoke-Prepare {
    Assert-Admin
    Write-Title 'Checking this PC'

    $sb   = Get-SecureBoot
    $ts   = Get-TestSigning
    $enc  = Get-EncryptionOn
    $hvci = Get-MemoryIntegrityRunning

    $sbText = if ($sb -eq $true) { 'ON' } elseif ($sb -eq $false) { 'off' } else { 'unknown' }
    $tsText = if ($ts -eq $true) { 'on' } elseif ($ts -eq $false) { 'OFF' } else { 'unknown' }
    $encText = if ($enc -eq $true) { 'ON' } elseif ($enc -eq $false) { 'off' } else { 'not used' }
    $hvText = if ($hvci -eq $true) { 'on' } elseif ($hvci -eq $false) { 'off' } else { 'unknown' }
    Write-Line "Secure Boot:            $sbText"
    Write-Line "Test signing:           $tsText"
    Write-Line "Drive encryption:       $encText"
    Write-Line "Memory integrity (HVCI): $hvText"

    if ($enc -eq $true) {
        Write-Note 'Drive encryption is on. Suspending it for the next 3 restarts so changing Secure Boot does not ask for the recovery key.'
        & manage-bde.exe -protectors -disable $env:SystemDrive -RebootCount 3 | Out-Null
        Write-Good 'Encryption protection suspended for 3 restarts (it turns itself back on).'
    }

    if ($ts -eq $true) {
        Write-Title 'Result'
        Write-Good 'Test signing is on. Next step:  .\tpctl.ps1 install'
        return
    }

    if ($sb -eq $true) {
        Write-Title 'Do this now'
        Write-Line 'Secure Boot is on, so Windows will not allow test signing. Turn Secure Boot off once:'
        Write-Line '  1. Restart. While it starts, press F2 repeatedly (on some Infinix models Del or F7) to open the BIOS.'
        Write-Line '  2. Security (or Boot) tab > Secure Boot > Disabled. If it is greyed out, first set an'
        Write-Line '     Administrator/Supervisor password on the Security tab, then Secure Boot becomes changeable.'
        Write-Line '  3. Save and exit (F10). Back in Windows, run  .\tpctl.ps1 prepare  again.'
        return
    }

    Write-Line 'Turning on test signing (allows drivers signed with your own certificate) ...'
    $out = (& bcdedit.exe /set testsigning on 2>&1 | Out-String)
    Write-Line $out.Trim()
    if (Get-TestSigning) {
        Write-Title 'Do this now'
        Write-Good 'Test signing is on. Restart Windows, then run  .\tpctl.ps1 install'
        $a = Read-Host 'Restart now? [Y/N]'
        if ($a -match '^[Yy]') { & shutdown.exe /r /t 3 }
    } else {
        Write-Bad 'Windows refused to turn on test signing. Secure Boot is probably still on: disable it in the BIOS (see above).'
    }
}

# ----------------------------------------------------------------------------- install

$NewDevCode = @'
using System;
using System.Runtime.InteropServices;
public static class TpNewDev {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwndParent, string hardwareId,
        string fullInfPath, uint installFlags, [MarshalAs(UnmanagedType.Bool)] out bool rebootRequired);
}
'@

function Invoke-Install {
    Assert-Admin
    Write-Title 'Installing the touchpad driver'

    foreach ($f in @($InfPath, (Join-Path $Here 'driver\htixtp.sys'), (Join-Path $Here 'driver\htixtp.cat'), $CerPath)) {
        if (-not (Test-Path $f)) { Write-Bad "Missing file: $f. Extract the whole TouchpadFix zip and run this from that folder."; exit 1 }
    }

    if (-not (Get-TestSigning)) {
        Write-Bad 'Test signing is off, so Windows would refuse to load this driver. Run  .\tpctl.ps1 prepare  first.'
        exit 1
    }

    $tp = Get-Touchpad
    if (-not $tp) { Write-Bad 'The touchpad (ACPI SYNA360/SYNA3602) was not found on this PC.'; exit 1 }

    $hwids = @(Get-DevProp $tp.InstanceId 'DEVPKEY_Device_HardwareIds')
    $hwid = $hwids | Where-Object { $OurIds -contains $_ } | Select-Object -First 1
    if (-not $hwid) { Write-Bad "None of the touchpad's hardware IDs ($($hwids -join ', ')) is in the driver INF."; exit 1 }
    Write-Line "Touchpad: $($tp.InstanceId)  (matching ID $hwid)"

    Write-Line 'Trusting the TouchpadFix test certificate (Trusted Root + Trusted Publishers) ...'
    Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $CerPath -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

    Write-Line 'Creating a restore point (may be skipped if one was made today) ...'
    try {
        Enable-ComputerRestore -Drive "$env:SystemDrive\" -ErrorAction Stop
        Checkpoint-Computer -Description 'Before TouchpadFix driver' -RestorePointType MODIFY_SETTINGS -ErrorAction Stop
        Write-Good 'Restore point created.'
    } catch {
        Write-Note "No restore point: $($_.Exception.Message)"
    }

    if (-not ('TpNewDev' -as [type])) { Add-Type -TypeDefinition $NewDevCode -ErrorAction Stop }

    Write-Line 'Installing (forcing it over Windows'' hidi2c driver for this one device) ...'
    $reboot = $false
    $ok = [TpNewDev]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero, $hwid, $InfPath, 1, [ref]$reboot)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()

    if (-not $ok) {
        Write-Bad ('Installation failed: error 0x{0:X8}' -f $err)
        Write-Note 'Fallback: Device Manager > Human Interface Devices > I2C HID Device (or the touchpad) > Update driver >'
        Write-Note '"Browse my computer" > "Let me pick" > "Have Disk" > choose driver\htixtp.inf in this folder.'
        exit 1
    }

    # Make sure the Windows touchpad switch is on for this user.
    $ptp = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\PrecisionTouchPad\Status'
    if (Test-Path $ptp) { Set-ItemProperty -Path $ptp -Name Enabled -Value 1 -Type DWord }

    # Keep Fast Startup off: every power-on must be a real cold start for the chip.
    Set-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power' -Name HiberbootEnabled -Value 0 -Type DWord

    Start-Sleep -Seconds 5
    $tp = Get-Touchpad
    Enable-TouchpadChildren $tp
    Write-Good 'Driver installed.'
    Write-Line ("Device status now: {0} (problem code {1}); our driver active: {2}" -f $tp.Status, (Get-ProblemCode $tp), (Test-OurDriver $tp))

    Write-Title 'Do this now'
    Write-Line 'Shut the laptop down completely (not restart) so the touchpad chip starts fresh with the new driver,'
    Write-Line 'then power it on, wait one minute, try the touchpad, and run:  .\tpctl.ps1 status'
    $a = Read-Host 'Shut down now? [Y/N]'
    if ($a -match '^[Yy]') { & shutdown.exe /s /t 3 }
}

# ----------------------------------------------------------------------------- status

function Show-Status([int]$LogLines = 25) {
    Write-Title 'Touchpad'
    $tp = Get-Touchpad
    if (-not $tp) { Write-Bad 'Touchpad device (ACPI SYNA360/SYNA3602) not found.'; return $null }

    $code = Get-ProblemCode $tp
    $ours = Test-OurDriver $tp
    $color = if ($code -eq 0) { 'Green' } else { 'Red' }
    Write-Host ("  {0}   status {1}, problem code {2}" -f $tp.InstanceId, $tp.Status, $code) -ForegroundColor $color
    $provider = Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverProvider'
    $infName  = Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverInfPath'
    $version  = Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverVersion'
    if ($ours) { Write-Good "Driver: TouchpadFix htixtp ($infName, version $version)" }
    else       { Write-Note "Driver: $provider $infName $version  (NOT the TouchpadFix driver yet)" }

    $kids = @(Get-TouchpadChildren $tp | Where-Object { $_.Present })
    foreach ($k in $kids) {
        Write-Line ("  {0,-6} code {1,-3} {2}" -f $k.Status, (Get-ProblemCode $k), $k.FriendlyName)
    }

    $ts = Get-TestSigning
    $tsText = if ($ts -eq $true) { 'on' } elseif ($ts -eq $false) { 'OFF' } else { 'unknown (run as administrator)' }
    Write-Line "Test signing: $tsText"

    $p = Get-Params $tp
    if ($ours -and $p -and $p.HtpVersion) {
        Write-Title 'Driver view'
        $vidpid = [uint32]$p.HtpChipVidPid
        Write-Line ("State {0}   mode {1}   input {2}   driver {3}" -f $p.HtpState, $p.HtpMode, $p.HtpInput, $p.HtpVersion)
        Write-Line ("Chip VID {0:X4} PID {1:X4} version {2:X4}, report descriptor {3} bytes" -f ($vidpid -shr 16), ($vidpid -band 0xFFFF), [uint32]$p.HtpChipVersion, $p.HtpReportDescLength)
        Write-Line ("I2C ok {0} / failed {1}   inits ok {2} / failed {3}   recoveries {4}   last error 0x{5:X8}" -f `
            $p.HtpStat_I2cOk, $p.HtpStat_I2cFail, $p.HtpStat_InitOk, $p.HtpStat_InitFail, $p.HtpStat_Recoveries, [uint32]$p.HtpStat_LastError)
        Write-Line ("Interrupts {0}   input reads {1}   reports delivered {2}   empty {3}   malformed {4}   dropped {5}" -f `
            $p.HtpStat_Interrupts, $p.HtpStat_InputReads, $p.HtpStat_ReportsDelivered, $p.HtpStat_EmptyReads, $p.HtpStat_BadReports, $p.HtpStat_ReportsDropped)
        Write-Line ("Windows reads pending {0}   feature get ok {1}/fail {2}   set ok {3}/fail {4}   precision touchpad configured by Windows: {5}" -f `
            $p.HtpStat_ReadRequests, $p.HtpStat_FeatureGetOk, $p.HtpStat_FeatureGetFail, $p.HtpStat_FeatureSetOk, $p.HtpStat_FeatureSetFail, [bool]$p.HtpPtpActivated)
        Write-Line ("Health checks {0} (failed {1})   polls {2}" -f $p.HtpStat_HealthChecks, $p.HtpStat_HealthFails, $p.HtpStat_PollReads)

        if ($p.HtpLog) {
            Write-Title "Driver log (last $LogLines lines)"
            @($p.HtpLog) | Select-Object -Last $LogLines | ForEach-Object { Write-Line $_ }
        }
    }

    Write-Title 'What to do next'
    if (-not $ours) {
        if ($ts -ne $true) { Write-Line 'Run  .\tpctl.ps1 prepare  then  .\tpctl.ps1 install' }
        else { Write-Line 'Run  .\tpctl.ps1 install' }
    } elseif ($code -eq 52) {
        Write-Bad 'Windows blocked the driver signature (code 52).'
        Write-Line 'Check that test signing is on (.\tpctl.ps1 prepare). If it is, turn off Windows Security > Device security >'
        Write-Line 'Core isolation > Memory integrity, restart, and check again.'
    } elseif ($code -ne 0) {
        if ($p -and $p.HtpState -eq 'NoResponse') {
            Write-Bad 'The driver loaded, but the chip never answered on I2C since power-on.'
            Write-Line 'Do one cold start: shut down, unplug the charger, unplug the battery connector for 2 minutes,'
            Write-Line 'reconnect, power on. Then run  .\tpctl.ps1 status'
        } else {
            Write-Bad "The touchpad device has problem code $code. Run  .\tpctl.ps1 collect  and send the report."
        }
    } elseif ($p) {
        $delivered = [int]$p.HtpStat_ReportsDelivered + [int]$p.HtpStat_ReportsQueued
        if ($p.HtpState -ne 'Ready') {
            Write-Note "Driver state is $($p.HtpState): it is waking the chip and will keep retrying."
            Write-Line 'If it stays like this for a minute, do the cold start (battery unplug) described in the guide.'
        } elseif ($delivered -gt 0) {
            Write-Good 'The chip is sending input and the driver is passing it to Windows.'
            if ($p.HtpMode -eq 'precision-touchpad') {
                Write-Line 'If the pointer still does not move:  .\tpctl.ps1 mode mouse'
            } else {
                Write-Line 'If the pointer still does not move: run  .\tpctl.ps1 collect  and send the report.'
            }
        } elseif ([int]$p.HtpStat_Interrupts -eq 0 -and $p.HtpInput -eq 'interrupt') {
            Write-Note 'No touchpad interrupt has arrived yet. Slide a finger on the touchpad for a few seconds, then run status again.'
            Write-Line 'If Interrupts stays 0 after touching:  .\tpctl.ps1 polling on'
        } else {
            Write-Note 'Touch the touchpad for a few seconds and run status again. If nothing changes:  .\tpctl.ps1 collect'
        }
    }
    return $tp
}

# ----------------------------------------------------------------------------- test

$InputTestCode = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

public class TpTestStat { public int Mouse; public int Hid; public long Movement; }

public class TpTestForm : Form {
    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTDEVICE { public ushort UsagePage; public ushort Usage; public uint Flags; public IntPtr Target; }
    [StructLayout(LayoutKind.Sequential)]
    struct RAWINPUTHEADER { public uint Type; public uint Size; public IntPtr Device; public IntPtr WParam; }
    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterRawInputDevices(RAWINPUTDEVICE[] devices, uint count, uint size);
    [DllImport("user32.dll", SetLastError = true)]
    static extern uint GetRawInputData(IntPtr raw, uint command, IntPtr data, ref uint size, uint headerSize);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetRawInputDeviceInfo(IntPtr device, uint command, StringBuilder data, ref uint size);

    public Dictionary<string, TpTestStat> Stats = new Dictionary<string, TpTestStat>();
    public int RegisterError;
    Dictionary<IntPtr, string> names = new Dictionary<IntPtr, string>();
    Label info; Timer timer; DateTime end;

    public TpTestForm(int seconds) {
        Text = "Touchpad test"; Width = 600; Height = 230; TopMost = true;
        StartPosition = FormStartPosition.CenterScreen;
        info = new Label(); info.Dock = DockStyle.Fill; info.Padding = new Padding(12);
        info.Font = new System.Drawing.Font("Segoe UI", 12);
        Controls.Add(info);
        end = DateTime.Now.AddSeconds(seconds);
        timer = new Timer(); timer.Interval = 200; timer.Tick += OnTick;
        ShowText(seconds);
    }

    protected override void OnHandleCreated(EventArgs e) {
        base.OnHandleCreated(e);
        RAWINPUTDEVICE[] devs = new RAWINPUTDEVICE[2];
        devs[0].UsagePage = 0x01; devs[0].Usage = 0x02; devs[0].Flags = 0x100; devs[0].Target = Handle;
        devs[1].UsagePage = 0x0D; devs[1].Usage = 0x05; devs[1].Flags = 0x100; devs[1].Target = Handle;
        if (!RegisterRawInputDevices(devs, 2, (uint)Marshal.SizeOf(typeof(RAWINPUTDEVICE))))
            RegisterError = Marshal.GetLastWin32Error();
        timer.Start();
    }

    void OnTick(object sender, EventArgs e) {
        int left = (int)Math.Ceiling((end - DateTime.Now).TotalSeconds);
        if (left <= 0) { timer.Stop(); Close(); return; }
        ShowText(left);
    }

    void ShowText(int left) {
        info.Text = "1. Move the USB mouse a little, then let go of it.\r\n2. Slide a finger on the TOUCHPAD and tap it.\r\n\r\nTime left: "
            + left + " s\r\nUSB mouse events: " + Reports(false) + "     Touchpad reports: " + Reports(true);
    }

    public static bool IsTouchpad(string name) {
        string u = name.ToUpperInvariant();
        return u.Contains("SYNA360") || u.Contains("HTIX5288") || u.Contains("VID_0911");
    }

    public int Reports(bool touchpad) {
        int n = 0;
        foreach (KeyValuePair<string, TpTestStat> kv in Stats)
            if (IsTouchpad(kv.Key) == touchpad) n += kv.Value.Mouse + kv.Value.Hid;
        return n;
    }

    protected override void WndProc(ref Message m) {
        if (m.Msg == 0x00FF) Record(m.LParam);
        base.WndProc(ref m);
    }

    void Record(IntPtr raw) {
        uint headerSize = (uint)Marshal.SizeOf(typeof(RAWINPUTHEADER));
        uint size = 0;
        GetRawInputData(raw, 0x10000003, IntPtr.Zero, ref size, headerSize);
        if (size == 0) return;
        IntPtr buf = Marshal.AllocHGlobal((int)size);
        try {
            if (GetRawInputData(raw, 0x10000003, buf, ref size, headerSize) == uint.MaxValue) return;
            RAWINPUTHEADER hdr = (RAWINPUTHEADER)Marshal.PtrToStructure(buf, typeof(RAWINPUTHEADER));
            string name = NameOf(hdr.Device);
            TpTestStat st;
            if (!Stats.TryGetValue(name, out st)) { st = new TpTestStat(); Stats[name] = st; }
            int body = (int)headerSize;
            if (hdr.Type == 0) {
                st.Mouse++;
                st.Movement += Math.Abs(Marshal.ReadInt32(buf, body + 12)) + Math.Abs(Marshal.ReadInt32(buf, body + 16));
            } else if (hdr.Type == 2) {
                st.Hid++;
            }
        } finally { Marshal.FreeHGlobal(buf); }
    }

    string NameOf(IntPtr device) {
        string n;
        if (names.TryGetValue(device, out n)) return n;
        if (device == IntPtr.Zero) { n = "(pointer input generated by Windows itself)"; }
        else {
            uint len = 0;
            GetRawInputDeviceInfo(device, 0x20000007, null, ref len);
            StringBuilder sb = new StringBuilder((int)len + 1);
            if (len > 0 && GetRawInputDeviceInfo(device, 0x20000007, sb, ref len) != uint.MaxValue) n = sb.ToString();
            else n = "(unknown device " + device.ToInt64() + ")";
        }
        names[device] = n;
        return n;
    }
}
'@

function Invoke-InputTest {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    if (-not ('TpTestForm' -as [type])) {
        Add-Type -TypeDefinition $InputTestCode -ReferencedAssemblies System.Windows.Forms, System.Drawing -ErrorAction Stop
    }
    [System.Windows.Forms.Application]::EnableVisualStyles()
    $form = New-Object TpTestForm 25
    [void]$form.ShowDialog()

    $tp = $form.Reports($true)
    $other = $form.Reports($false)
    Write-Title 'Input test result'
    foreach ($kv in $form.Stats.GetEnumerator()) {
        $kind = if ([TpTestForm]::IsTouchpad($kv.Key)) { 'TOUCHPAD' } else { 'other   ' }
        Write-Line ("{0}  HID reports {1,5}  mouse events {2,5}  movement {3,7}  {4}" -f $kind, $kv.Value.Hid, $kv.Value.Mouse, $kv.Value.Movement, $kv.Key)
    }
    if ($form.RegisterError -ne 0) { Write-Note ("Raw input registration failed (error {0})." -f $form.RegisterError) }
    if ($other -eq 0) { Write-Note 'No USB mouse events either: the test was not listening. Run it again and move the USB mouse first.' }
    if ($tp -gt 0) { Write-Good "Touchpad input reached Windows ($tp reports)." }
    else { Write-Bad 'No touchpad input reached Windows.' }
}

# ----------------------------------------------------------------------------- mode / polling / restart

function Invoke-Mode([string]$Which) {
    Assert-Admin
    $tp = Get-Touchpad
    if (-not $tp -or -not (Test-OurDriver $tp)) { Write-Bad 'The TouchpadFix driver is not installed on the touchpad.'; exit 1 }
    $map = @{ 'auto' = 0; 'full' = 1; 'mouse' = 2 }
    if (-not $map.ContainsKey($Which)) { Write-Bad 'Use: .\tpctl.ps1 mode auto|full|mouse'; exit 1 }
    Set-DeviceParam $tp 'HtpDescriptorMode' $map[$Which]
    Set-DeviceParam $tp 'HtpAutoMouse' 0
    Write-Good "Mode set to $Which."
    Restart-Touchpad $tp
    Start-Sleep -Seconds 2
    Enable-TouchpadChildren $tp
    Show-Status 12 | Out-Null
}

function Invoke-Polling([string]$Which) {
    Assert-Admin
    $tp = Get-Touchpad
    if (-not $tp -or -not (Test-OurDriver $tp)) { Write-Bad 'The TouchpadFix driver is not installed on the touchpad.'; exit 1 }
    $map = @{ 'off' = 0; 'on' = 1; 'both' = 2 }
    if (-not $map.ContainsKey($Which)) { Write-Bad 'Use: .\tpctl.ps1 polling off|on|both'; exit 1 }
    Set-DeviceParam $tp 'HtpPolling' $map[$Which]
    Write-Good "Polling set to $Which."
    Restart-Touchpad $tp
    Show-Status 12 | Out-Null
}

# ----------------------------------------------------------------------------- collect

function Invoke-Collect {
    $dir = Join-Path ([Environment]::GetFolderPath('Desktop')) 'TouchpadFix'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $file = Join-Path $dir ('report_{0:yyyyMMdd_HHmmss}.txt' -f (Get-Date))
    Start-Transcript -Path $file | Out-Null
    try {
        $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        Write-Line ("Windows {0} build {1}.{2}" -f $os.DisplayVersion, $os.CurrentBuild, $os.UBR)
        $tp = Show-Status 64
        if ($tp) {
            Write-Title 'All driver parameters'
            $p = Get-Params $tp
            if ($p) {
                $p.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' -and $_.Name -ne 'HtpLog' -and $_.Name -notlike 'HtpCache*' } |
                    ForEach-Object { Write-Line ("{0} = {1}" -f $_.Name, $_.Value) }
            }
            Write-Title 'Device properties'
            foreach ($key in 'DEVPKEY_Device_HardwareIds', 'DEVPKEY_Device_Service', 'DEVPKEY_Device_LowerFilters', 'DEVPKEY_Device_UpperFilters', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion', 'DEVPKEY_Device_ProblemStatus') {
                Write-Line ("{0}: {1}" -f $key, ((Get-DevProp $tp.InstanceId $key) -join ', '))
            }
            Write-Title 'Recent PnP events'
            try {
                Get-WinEvent -LogName 'Microsoft-Windows-Kernel-PnP/Configuration' -MaxEvents 300 -ErrorAction Stop |
                    Where-Object { $_.Message -like '*SYNA36*' -or $_.Message -like '*htixtp*' } | Select-Object -First 12 |
                    ForEach-Object { Write-Line ('{0:yyyy-MM-dd HH:mm:ss} id {1}: {2}' -f $_.TimeCreated, $_.Id, (($_.Message -split "`r?`n")[0])) }
            } catch { Write-Line 'PnP event log not readable.' }
        }
        Write-Line ("Secure Boot: {0}   Memory integrity running: {1}" -f (Get-SecureBoot), (Get-MemoryIntegrityRunning))
    } finally {
        Stop-Transcript | Out-Null
    }
    Write-Good "Report saved: $file"
}

# ----------------------------------------------------------------------------- uninstall

function Invoke-Uninstall {
    Assert-Admin
    Write-Title 'Removing the TouchpadFix driver'
    $tp = Get-Touchpad
    $infs = @()
    if ($tp) {
        $bound = Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverInfPath'
        if ($bound -and (Test-OurDriver $tp)) { $infs += $bound }
    }
    Get-ChildItem "$env:windir\INF\oem*.inf" -ErrorAction SilentlyContinue | ForEach-Object {
        if (Select-String -Path $_.FullName -Pattern 'htixtp' -SimpleMatch -Quiet) { $infs += $_.Name }
    }
    $infs = $infs | Sort-Object -Unique
    if ($infs.Count -eq 0) { Write-Note 'No TouchpadFix driver package found.' }
    foreach ($i in $infs) {
        Write-Line "pnputil /delete-driver $i /uninstall /force"
        & pnputil.exe /delete-driver $i /uninstall /force | ForEach-Object { Write-Line $_ }
    }
    & pnputil.exe /scan-devices | Out-Null
    Start-Sleep -Seconds 3
    $tp = Get-Touchpad
    if ($tp) {
        Write-Line ("Touchpad now uses: {0} {1}" -f (Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverProvider'), (Get-DevProp $tp.InstanceId 'DEVPKEY_Device_DriverInfPath'))
    }
    Write-Line 'Test signing stays on until you turn it off:  bcdedit /set testsigning off  (then restart).'
}

# ----------------------------------------------------------------------------- main

switch ($Command) {
    'prepare'   { Invoke-Prepare }
    'install'   { Invoke-Install }
    'status'    { Show-Status 25 | Out-Null }
    'test'      { Invoke-InputTest }
    'mode'      { Invoke-Mode ($Value + '').ToLowerInvariant() }
    'polling'   { Invoke-Polling ($Value + '').ToLowerInvariant() }
    'restart'   { Assert-Admin; $tp = Get-Touchpad; if ($tp) { Restart-Touchpad $tp; Show-Status 12 | Out-Null } }
    'collect'   { Invoke-Collect }
    'uninstall' { Invoke-Uninstall }
    default {
        Get-Content $PSCommandPath -TotalCount 20 | Select-Object -Skip 1 | ForEach-Object { Write-Host $_ }
    }
}
