param([Parameter(Mandatory)][string]$Installer)
$ErrorActionPreference = 'Stop'
$installerFile = Get-Item -LiteralPath $Installer
$checksums = Get-Content -LiteralPath (Join-Path $installerFile.DirectoryName 'SHA256SUMS')
$entry = @($checksums | Where-Object { $_ -cmatch ('^[0-9a-f]{64}  ' + [regex]::Escape($installerFile.Name) + '$') })
if ($entry.Count -ne 1 -or (Get-FileHash $installerFile.FullName).Hash -ne $entry[0].Substring(0, 64)) {
    throw 'The installer checksum did not match the package manifest.'
}

$directory = Join-Path $env:LOCALAPPDATA 'Programs\Exordos\Workspace Native Preview'
$executable = Join-Path $directory 'bin\workspace-native.exe'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Workspace Native Preview.lnk'
$registration = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\com.exordos.workspace.native.preview_is1'
if ((Test-Path $directory) -or (Test-Path $registration) -or (Test-Path $shortcut)) {
    throw 'The installer test requires a fresh user profile.'
}
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-')
$sentinel = Join-Path $directory ('unowned-' + [guid]::NewGuid() + '.txt')
try {
    $setup = Start-Process -FilePath $installerFile.FullName -ArgumentList $arguments -PassThru -Wait
    if ($setup.ExitCode -ne 0) { throw "Setup returned $($setup.ExitCode)." }
    $product = Get-ItemProperty -LiteralPath $registration
    if ($product.DisplayName -notlike 'Workspace Native Preview*' -or !$product.DisplayVersion) {
        throw 'Missing preview identity in the current user installation record.'
    }
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    if ($link.TargetPath -ne $executable) { throw 'The Start menu shortcut has an unexpected target.' }
    $shell = New-Object -ComObject Shell.Application
    $linkEntry = $shell.Namespace((Split-Path $shortcut)).ParseName((Split-Path $shortcut -Leaf))
    if ($linkEntry.ExtendedProperty('System.AppUserModel.ID') -cne 'com.exordos.workspace.native.preview') {
        throw 'The Start menu shortcut has an unexpected application ID.'
    }
    Set-Content -LiteralPath $sentinel -Value 'keep'
    & "$PSScriptRoot/windows-window.ps1" -Executable $link.TargetPath
    Write-Output "Verified per-user installation of $($product.DisplayName) $($product.DisplayVersion)."
} finally {
    if (Test-Path $registration) {
        $uninstaller = (Get-ItemProperty -LiteralPath $registration).UninstallString.Trim('"')
        if ((Split-Path $uninstaller) -ne $directory) { throw 'Unexpected preview uninstaller location.' }
        $removal = Start-Process -FilePath $uninstaller -ArgumentList $arguments -PassThru -Wait
        if ($removal.ExitCode -ne 0) { throw "Uninstall returned $($removal.ExitCode)." }
    }
}
if ((Test-Path $executable) -or (Test-Path $registration) -or (Test-Path $shortcut) -or
    !(Test-Path -LiteralPath $sentinel) -or @(Get-ChildItem -LiteralPath $directory -Force).Count -ne 1) {
    throw 'Uninstall did not remove exactly the installed files, registration and shortcut.'
}
Remove-Item -LiteralPath $sentinel
Remove-Item -LiteralPath $directory
Write-Output 'Verified uninstall and preservation of an unrelated file.'
