param(
    [string]$Port = "COM5",
    [int]$Count = 50,
    [int]$ObserveSeconds = 12
)

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.WriteTimeout = 3000
$serial.ReadTimeout = 50
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.Open()
$received = [System.Text.StringBuilder]::new()

try {
    Start-Sleep -Milliseconds 500
    [void]$received.Append($serial.ReadExisting())
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    for ($index = 0; $index -lt $Count; $index++) {
        $serial.WriteLine("state $($index % 20)")
        [void]$received.Append($serial.ReadExisting())
    }
    $watch.Stop()

    $deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        [void]$received.Append($serial.ReadExisting())
        Start-Sleep -Milliseconds 20
    }
} finally {
    $serial.Close()
}

$text = $received.ToString()
$acks = ([regex]::Matches($text, 'State: ')).Count
$queueFull = ([regex]::Matches($text, 'state request queue full')).Count
$resets = ([regex]::Matches($text, 'rst:0x')).Count
$applied = ([regex]::Matches($text, 'animation state target=')).Count
Write-Output ("STATE_BURST sent={0} send_ms={1:F2} acks={2} applied={3} queue_full={4} resets={5}" -f `
    $Count, $watch.Elapsed.TotalMilliseconds, $acks, $applied, $queueFull, $resets)
Write-Output $text
