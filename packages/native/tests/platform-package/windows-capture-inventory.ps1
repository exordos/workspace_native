$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$device = [Windows.Media.Devices.MediaDevice,Windows.Media.Devices,ContentType=WindowsRuntime]
$info = [Windows.Devices.Enumeration.DeviceInformation,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
$collection = [Windows.Devices.Enumeration.DeviceInformationCollection,Windows.Devices.Enumeration,ContentType=WindowsRuntime]
$toTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetGenericArguments().Count -eq 1 -and
    $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
foreach ($entry in @(@('audio', $device::GetAudioCaptureSelector()), @('video', $device::GetVideoCaptureSelector()))) {
    $operation = $info::FindAllAsync([string]$entry[1])
    $task = $toTask.MakeGenericMethod($collection).Invoke($null, @($operation))
    if (!$task.Wait(10000)) { throw 'Native device enumeration timed out' }
    Write-Host "Windows available $($entry[0]) capture devices: $($task.Result.Count)"
}
