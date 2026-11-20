param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeWindow {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out Rect rect);
}
"@
$app = Start-Process -FilePath $Executable -PassThru
try {
    $deadline = (Get-Date).AddSeconds(90)
    do {
        Start-Sleep -Milliseconds 250
        $app.Refresh()
        if ($app.HasExited) { throw "The packaged process exited with code $($app.ExitCode)." }
        $window = $app.MainWindowHandle
        $bounds = New-Object NativeWindow+Rect
        if ($window -ne [IntPtr]::Zero -and [NativeWindow]::IsWindowVisible($window) -and
            [NativeWindow]::GetClientRect($window, [ref]$bounds) -and $bounds.Right -ge 400 -and $bounds.Bottom -ge 300) {
            Write-Output "Observed packaged Windows process $($app.Id) with visible client area $($bounds.Right) x $($bounds.Bottom)."
            return
        }
    } while ((Get-Date) -lt $deadline)
    throw "The packaged process did not present its main window."
} finally {
    if (!$app.HasExited) { Stop-Process -Id $app.Id -Force }
}
