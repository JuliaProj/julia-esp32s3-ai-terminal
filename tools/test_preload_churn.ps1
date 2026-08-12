param(
    [string]$Port = "COM5",
    [int]$ObserveSeconds = 120,
    [switch]$Reset,
    [switch]$VoiceTest
)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.WriteTimeout = 3000
$serial.ReadTimeout = 50
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.Open()
$received = [System.Text.StringBuilder]::new()

try {
    if ($Reset) {
        $serial.RtsEnable = $true
        Start-Sleep -Milliseconds 100
        $serial.RtsEnable = $false
        Start-Sleep -Seconds 8
    } else {
        Start-Sleep -Milliseconds 500
    }
    [void]$received.Append($serial.ReadExisting())
    $serial.WriteLine('preload-test')
    $deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
    $voiceAt = [DateTime]::UtcNow.AddSeconds(5)
    $voiceSent = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        [void]$received.Append($serial.ReadExisting())
        if ($VoiceTest -and !$voiceSent -and [DateTime]::UtcNow -ge $voiceAt) {
            $serial.WriteLine('voice-test')
            $voiceSent = $true
        }
        Start-Sleep -Milliseconds 20
    }
} finally {
    $serial.Close()
}

$text = $received.ToString()
$lines = $text -split "`r?`n" | Where-Object {
    $_ -match 'Preload churn|Voice test|JULIA_AUDIO|AVATAR_PRELOAD|AVATAR_CACHE|AVATAR_ANIM: (metrics|display CRC)|panic|watchdog|rst:0x'
}
$crcFail = ([regex]::Matches($text, 'display CRC.*FAIL')).Count
$panic = ([regex]::Matches($text, 'Guru Meditation|panic|Task watchdog')).Count
$resetCount = ([regex]::Matches($text, 'rst:0x')).Count
Write-Output "PRELOAD_CHURN crc_fail=$crcFail panic=$panic resets=$resetCount"
$lines | Write-Output
$allLines = $text -split "`r?`n"
for ($index = 0; $index -lt $allLines.Count; $index++) {
    if ($allLines[$index] -match 'Guru Meditation') {
        $start = [Math]::Max(0, $index - 5)
        $end = [Math]::Min($allLines.Count - 1, $index + 35)
        Write-Output '--- PANIC CONTEXT ---'
        $allLines[$start..$end] | Write-Output
    }
}
