param([int]$ApplicationPid, [string]$Mode, [string]$Origin)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Windows.Forms
$app = Get-Process -Id $ApplicationPid
$limit = (Get-Date).AddSeconds(25)
do {
    $app.Refresh()
    if ($app.HasExited) { throw 'Application exited before the login window appeared.' }
    $handle = $app.MainWindowHandle
    if ($handle -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 100
} while ((Get-Date) -lt $limit)
$root = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
function Control([string]$Name, $Type) {
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $Type))
    $deadline = (Get-Date).AddSeconds(15)
    do {
        $results = @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) |
            Where-Object { $_.Current.IsEnabled -and !$_.Current.IsOffscreen })
        if ($results.Count -eq 1) { return $results[0] }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    throw "The application did not expose an enabled $Name control."
}
function Key([string]$Value) {
    [System.Windows.Forms.SendKeys]::SendWait($Value)
}
function Fill([string]$Name, [string]$Value) {
    $item = Control $Name ([System.Windows.Automation.ControlType]::Edit)
    $item.SetFocus()
    Set-Clipboard -Value $Value
    Key '^a'
    Key '^v'
}
function Press([string]$Name) {
    $item = Control $Name ([System.Windows.Automation.ControlType]::Button)
    $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
switch ($Mode) {
    'login' {
        Fill 'Server address' $Origin
        Press 'Next'
        Fill 'Email or login' 'package-member'
        Fill 'Password' 'package-demo'
        Press 'Next'
        Press 'Login'
    }
    'logout' {
        Press 'Profile'
        Press 'Sign out'
        [void](Control 'Server address' ([System.Windows.Automation.ControlType]::Edit))
    }
    'quit' {
        (New-Object -ComObject WScript.Shell).AppActivate($ApplicationPid) | Out-Null
        Key '^q'
    }
    default { throw "Unknown mode $Mode" }
}
