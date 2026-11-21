$ErrorActionPreference = 'Stop'
$root = Join-Path $env:RUNNER_TEMP ('preview-msi-' + [guid]::NewGuid())
$stage = Join-Path $root 'stage'
$install = Join-Path $env:LOCALAPPDATA 'Workspace Native Preview'
$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'Workspace Native Preview.lnk'
if ((Test-Path $install) -or (Test-Path $shortcut)) { throw 'A clean runner is required.' }
New-Item -ItemType Directory -Path "$stage/bin" -Force | Out-Null
Copy-Item "$env:SystemRoot/System32/where.exe" "$stage/bin/workspace-native.exe"
Set-Content "$stage/old-only.txt" 'from version one'
$env:WORKSPACE_MSI_STAGE=$stage
$env:WORKSPACE_MSI_ARCH = if ($env:RUNNER_ARCH -eq 'ARM64') { 'arm64' } else { 'x64' }
function Build-Msi([string]$Version) {
  $env:WORKSPACE_MSI_VERSION=$Version
  $out = Join-Path $root $Version
  & $env:WORKSPACE_CPACK --config packages/native/packaging/windows-msi.cmake -B $out --verbose | Out-Host
  if ($LASTEXITCODE -ne 0) {
    Get-ChildItem $out -Recurse -Filter wix.log | ForEach-Object { Get-Content $_.FullName | Select-Object -Last 45 | Out-Host }
    throw 'CPack could not build the MSI.'
  }
  (Get-Item "$out/workspace-native-windows-$env:WORKSPACE_MSI_ARCH-setup.msi").FullName
}
function Invoke-Msi([string]$Action,[string]$Package) {
  $p=Start-Process msiexec.exe -ArgumentList @($Action, ('"' + $Package + '"'), '/qn','/norestart') -PassThru -Wait
  if ($p.ExitCode) {throw "msiexec failed with $($p.ExitCode)."}
}
$initial=Build-Msi '1.0.0'
Invoke-Msi '/i' $initial
if (!(Test-Path "$install/old-only.txt")) {throw 'Missing original file.'}
Set-Content "$install/user-file.txt" 'user data'
Remove-Item "$stage/old-only.txt"
Set-Content "$stage/new-only.txt" 'from version two'
$final=Build-Msi '2.0.0'
Invoke-Msi '/i' $final
if ((Test-Path "$install/old-only.txt") -or !(Test-Path "$install/new-only.txt") -or (Get-Content "$install/user-file.txt") -ne 'user data') {throw 'Upgrade inventory mismatch.'}
if (!(Test-Path $shortcut)) {throw 'Missing current-user shortcut.'}
Invoke-Msi '/x' $final
if ((Test-Path $shortcut) -or @(Get-ChildItem $install -Force).Count -ne 1 -or !(Test-Path "$install/user-file.txt")) {throw 'Uninstall inventory mismatch.'}
Write-Output 'MSI removed obsolete files on upgrade, preserved the unrelated file and uninstalled cleanly.'
