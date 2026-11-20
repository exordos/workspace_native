param(
    [Parameter(Mandatory)][string]$Installer,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [switch]$WindowOnly
)
$ErrorActionPreference = 'Stop'
$installerFile = Get-Item -LiteralPath $Installer
$checksums = Get-Content -LiteralPath (Join-Path $installerFile.DirectoryName 'SHA256SUMS')
$entry = @($checksums | Where-Object { $_ -cmatch ('^[0-9a-f]{64}  ' + [regex]::Escape($installerFile.Name) + '$') })
if ($entry.Count -ne 1 -or (Get-FileHash $installerFile.FullName).Hash -ne $entry[0].Substring(0, 64)) {
    throw 'The installer checksum did not match the package manifest.'
}

$directory = Join-Path $env:LOCALAPPDATA 'Workspace Native Preview'
$executable = Join-Path $directory 'bin\workspace-native.exe'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Workspace Native Preview.lnk'
$windowsInstaller = New-Object -ComObject WindowsInstaller.Installer
$database = $windowsInstaller.OpenDatabase($installerFile.FullName, 0)
$query = $database.OpenView("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = 'ProductCode'")
$query.Execute()
$productCode = $query.Fetch().StringData(1)
$query.Close()
if ((Test-Path $directory) -or $windowsInstaller.ProductState($productCode) -ne -1 -or (Test-Path $shortcut)) {
    throw 'The installer test requires a fresh user profile.'
}
$arguments = @(('"' + $installerFile.FullName + '"'), '/qn', '/norestart')
$sentinel = Join-Path $directory ('unowned-' + [guid]::NewGuid() + '.txt')
try {
    $setup = Start-Process msiexec.exe -ArgumentList (@('/i') + $arguments) -PassThru -Wait
    if ($setup.ExitCode -ne 0) { throw "Setup returned $($setup.ExitCode)." }
    $name = $windowsInstaller.ProductInfo($productCode, 'ProductName')
    $version = $windowsInstaller.ProductInfo($productCode, 'VersionString')
    $assignment = $windowsInstaller.ProductInfo($productCode, 'AssignmentType')
    if ($windowsInstaller.ProductState($productCode) -ne 5 -or $assignment -ne '0' -or
        $name -notlike 'Workspace Native Preview*' -or !$version) {
        throw "Unexpected MSI product registration for $name $version, assignment $assignment."
    }
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    if ($link.TargetPath -ne $executable) { throw 'The Start menu shortcut has an unexpected target.' }
    $shell = New-Object -ComObject Shell.Application
    $linkEntry = $shell.Namespace((Split-Path $shortcut)).ParseName((Split-Path $shortcut -Leaf))
    if ($linkEntry.ExtendedProperty('System.AppUserModel.ID') -cne 'com.exordos.workspace.native.preview') {
        throw 'The Start menu shortcut has an unexpected application ID.'
    }
    Set-Content -LiteralPath $sentinel -Value 'keep'
    & "$PSScriptRoot/windows-window.ps1" -Executable $link.TargetPath -Architecture $Architecture -WindowOnly:$WindowOnly
    Write-Output "Verified per-user installation of $name $version."
} finally {
    if ($windowsInstaller.ProductState($productCode) -eq 5) {
        $removal = Start-Process msiexec.exe -ArgumentList (@('/x') + $arguments) -PassThru -Wait
        if ($removal.ExitCode -ne 0) { throw "Uninstall returned $($removal.ExitCode)." }
    }
}
if ((Test-Path $executable) -or ($windowsInstaller.ProductState($productCode) -ne -1) -or (Test-Path $shortcut) -or
    !(Test-Path -LiteralPath $sentinel) -or @(Get-ChildItem -LiteralPath $directory -Force).Count -ne 1) {
    throw 'Uninstall did not remove exactly the installed files, registration and shortcut.'
}
Remove-Item -LiteralPath $sentinel
Remove-Item -LiteralPath $directory
Write-Output 'Verified uninstall and preservation of an unrelated file.'
