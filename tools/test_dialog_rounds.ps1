param(
    [string]$Port = "COM5",
    [int]$Rounds = 10,
    [int]$ObserveSeconds = 65
)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.WriteTimeout = 3000
$serial.ReadTimeout = 50
$serial.DtrEnable = $false
$serial.RtsEnable = $true
$serial.Open()
Start-Sleep -Milliseconds 100
$serial.RtsEnable = $false
$received = [System.Text.StringBuilder]::new()

try {
    Start-Sleep -Seconds 10
    [void]$received.Append($serial.ReadExisting())
    $serial.WriteLine('status')
    $serial.WriteLine('state 13')
    Start-Sleep -Milliseconds 600
    for ($round = 0; $round -lt $Rounds; $round++) {
        foreach ($phase in 1, 2, 3, 0) {
            $serial.WriteLine("phase $phase")
            $deadline = [DateTime]::UtcNow.AddMilliseconds(600)
            while ([DateTime]::UtcNow -lt $deadline) {
                [void]$received.Append($serial.ReadExisting())
                Start-Sleep -Milliseconds 20
            }
        }
    }
    $serial.WriteLine('status')
    $deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        [void]$received.Append($serial.ReadExisting())
        Start-Sleep -Milliseconds 20
    }
} finally {
    $serial.Close()
}

$text = $received.ToString()
$crcFail = ([regex]::Matches($text, 'display CRC.*FAIL')).Count
$panic = ([regex]::Matches($text, 'Guru Meditation|Task watchdog')).Count
$resets = ([regex]::Matches($text, 'rst:0x')).Count
$completed = ([regex]::Matches($text, 'switch complete clip=')).Count
Write-Output "DIALOG_ROUNDS rounds=$Rounds completed=$completed crc_fail=$crcFail panic=$panic resets=$resets"
$text -split "`r?`n" | Where-Object {
    $_ -match '^STATUS|Dialog phase:|State:|AVATAR_ANIM: (switch first visible|switch complete|display CRC|metrics)|AVATAR_PRELOAD: (loaded|load failed)|Guru Meditation|watchdog|rst:0x'
} | Write-Output
