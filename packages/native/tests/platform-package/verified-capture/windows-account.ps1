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
    if ($Name -eq 'Password') {
        $condition = [System.Windows.Automation.AndCondition]::new(
            [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, $Type),
            [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::IsPasswordProperty, $true))
    }
    $deadline = (Get-Date).AddSeconds(15)
    do {
        $results = @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) |
            Where-Object { $_.Current.IsEnabled -and !$_.Current.IsOffscreen })
        # Qt WebEngine can expose the same Qt control through many fragments.
        # Coalesce only identical owning PIDs, identifiers and screen rectangles.
        if ($results.Count -gt 1 -and $results[0].Current.AutomationId) {
            $presentations = @($results | ForEach-Object {
                "$($_.Current.ProcessId)/$($_.Current.AutomationId)/$($_.Current.BoundingRectangle)"
            } | Select-Object -Unique)
            if ($presentations.Count -eq 1) { $results = @($results[0]) }
        }
        if ($results.Count -eq 1) {
            if ($Name -eq 'Password') {
                $field = $results[0].Current
                if ($field.Name -ne $Name -and (!$field.LabeledBy -or $field.LabeledBy.Current.Name -ne $Name)) {
                    throw 'The protected input has neither an accessible name nor its visible Password label.'
                }
            }
            return $results[0]
        }
        Start-Sleep -Milliseconds 100
    } while ((Get-Date) -lt $deadline)
    $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $allowed = @('Server address','Email or login','Password','Next','Login','Profile','Sign out',
        'Could not load organization settings. Check the server address and try again.')
    foreach ($item in $all) {
        $c = $item.Current
        if ($c.Name -in $allowed -or $c.IsPassword -or $c.ControlType -eq [System.Windows.Automation.ControlType]::Edit) {
            $label = if ($c.Name -in $allowed) { $c.Name } else { 'other field' }
            Write-Output "$label $($c.ControlType.ProgrammaticName) password=$($c.IsPassword) enabled=$($c.IsEnabled) offscreen=$($c.IsOffscreen)" | Out-Host
        }
    }
    throw "The application did not expose an enabled $Name control."
}
function Key([string]$Value) {
    [System.Windows.Forms.SendKeys]::SendWait($Value)
}
function Fill([string]$Name, [string]$Value) {
    $item = Control $Name ([System.Windows.Automation.ControlType]::Edit)
    $item.SetFocus()
    Key '^a'
    if ($Name -eq 'Password') {
        Key $Value
    } else {
        Set-Clipboard -Value $Value
        Key '^v'
    }
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
    'choose-file' {
        Press 'Attach files'
        $type = [System.Windows.Automation.AutomationElement]
        $dialogFilter = [System.Windows.Automation.AndCondition]::new(
            [System.Windows.Automation.PropertyCondition]::new($type::ClassNameProperty, '#32770'),
            [System.Windows.Automation.PropertyCondition]::new($type::ProcessIdProperty, $ApplicationPid))
        $deadline = (Get-Date).AddSeconds(15)
        do {
            $dialog = $type::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $dialogFilter)
            if ($dialog) { break }
            Start-Sleep -Milliseconds 100
        } while ((Get-Date) -lt $deadline)
        if (!$dialog) { throw 'The Microsoft common file chooser did not appear.' }
        $typeFilter = [System.Windows.Automation.PropertyCondition]::new($type::ControlTypeProperty,
            [System.Windows.Automation.ControlType]::Edit)
        $edit = @($dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants, $typeFilter) |
            Where-Object { $_.Current.Name -eq 'File name:' -or $_.Current.AutomationId -eq '1148' })
        if ($edit.Count -ne 1) { throw 'The native file name field was not unique.' }
        $edit[0].SetFocus()
        $edit[0].GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Origin)
        Key '{ENTER}'
        Write-Output 'Selected the synthetic file in a Microsoft common file dialog.'
    }
    'join-call' {
        Press 'General Chat: Cassi hosted channel'
        Press 'Join call'
    }
    'close-call' { Press 'Close call' }
    'update' {
        Press 'Profile'
        Press 'App version'
        Press 'Check for updates'
        [void](Control 'Updates are not available for this platform.' ([System.Windows.Automation.ControlType]::Text))
        Press 'Close'
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
