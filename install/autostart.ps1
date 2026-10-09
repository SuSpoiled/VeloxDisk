# Register the VeloxDisk auto-start scheduled task (Task Scheduler COM API).
# Task XML is used on purpose: `schtasks /Create /TR` parses its command line
# and breaks paths that contain spaces (Execute ends up as "C:\Program"),
# which makes the task fail at logon with "file not found" (0x80070002).
#
# Notes on the XML:
# - No <UserId>: the task then runs as the user that registers it, with
#   InteractiveToken + HighestAvailable. Hard-coding "Administrator" would
#   break on systems where the built-in Administrator account is disabled
#   (the Windows 10/11 default).
# - <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>: no limit. The scheduler's
#   implicit default is 72 hours, which would kill the tray daemon on
#   machines that stay logged on for more than 3 days.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File autostart.ps1 -ExePath "C:\...\vd_gui.exe"
param([Parameter(Mandatory = $true)][string]$ExePath)

$cmd = [System.Security.SecurityElement]::Escape($ExePath)
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Triggers><LogonTrigger><Enabled>true</Enabled></LogonTrigger></Triggers>
  <Principals><Principal id="Author"><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings><ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings>
  <Actions Context="Author"><Exec><Command>$cmd</Command><Arguments>--tray</Arguments></Exec></Actions>
</Task>
"@

$ts = New-Object -ComObject Schedule.Service
$ts.Connect()
$folder = $ts.GetFolder('\')
try { $folder.DeleteTask('VeloxDisk', 0) | Out-Null } catch { }
try {
    $folder.RegisterTask('VeloxDisk', $xml, 2, $null, $null, 0, $null) | Out-Null
    Write-Output 'VeloxDisk autostart task registered.'
    exit 0
} catch {
    Write-Error ('RegisterTask failed: ' + $_.Exception.Message)
    exit 1
}
