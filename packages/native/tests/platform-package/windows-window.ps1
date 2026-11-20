param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)
$ErrorActionPreference = "Stop"
$machine = if ($Architecture -eq 'arm64') { 0xaa64 } else { 0x8664 }
function Assert-PeMachine([string]$Path) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid DOS header in $Path" }
        $reader.BaseStream.Position = 0x3c
        $header = $reader.ReadUInt32()
        $reader.BaseStream.Position = $header
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $machine) {
            throw "Expected a native $Architecture PE image in $Path"
        }
    } finally { $reader.Dispose() }
}
Assert-PeMachine $Executable
if ($Architecture -eq 'arm64') {
    $bin = Split-Path $Executable
    foreach ($image in @('Qt6Core.dll', 'Qt6WebEngineCore.dll', 'QtWebEngineProcess.exe')) {
        Assert-PeMachine (Join-Path $bin $image)
    }
}
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out Rect rect);
}
"@
$app = Start-Process -FilePath $Executable -PassThru
try {
    [UInt16]$processMachine = 0
    [UInt16]$nativeMachine = 0
    if (-not [NativeWindow]::IsWow64Process2($app.Handle, [ref]$processMachine, [ref]$nativeMachine) -or
        $processMachine -ne 0 -or $nativeMachine -ne $machine) {
        throw "Expected the running $Architecture client to execute natively on matching hardware."
    }
    Write-Output "Confirmed native $Architecture PE, host and application process."
    $deadline = (Get-Date).AddSeconds(90)
    do {
        Start-Sleep -Milliseconds 250
        $app.Refresh()
        if ($app.HasExited) { throw "The packaged process exited with code $($app.ExitCode)." }
        $window = $app.MainWindowHandle
        $bounds = New-Object NativeWindow+Rect
        if ($window -ne [IntPtr]::Zero -and [NativeWindow]::IsWindowVisible($window) -and
            [NativeWindow]::GetClientRect($window, [ref]$bounds) -and $bounds.Right -ge 400 -and $bounds.Bottom -ge 300) {
            $bin = (Get-Item $Executable).Directory.FullName
            foreach ($name in @('MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll')) {
                $loaded = @($app.Modules | Where-Object { $_.ModuleName -eq $name })
                if ($loaded.Count -ne 1 -or (Split-Path $loaded[0].FileName) -ne $bin) {
                    throw "The application did not load its own $name runtime."
                }
            }
            Write-Output "Confirmed application-local Visual C++ runtime."
            Write-Output "Observed packaged Windows process $($app.Id) with visible client area $($bounds.Right) x $($bounds.Bottom)."
            return
        }
    } while ((Get-Date) -lt $deadline)
    throw "The packaged process did not present its main window."
} finally {
    if (!$app.HasExited) { Stop-Process -Id $app.Id -Force }
}
