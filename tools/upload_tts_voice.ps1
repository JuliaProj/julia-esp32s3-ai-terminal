param(
    [string]$Port = "COM5",
    [string]$VoiceFile = "waveshare_demo/ESP-IDF/ESP32-S3-LCD-1.85-Test/components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat"
)

$expectedBytes = 2938039
$expectedCrc = "a085d260"
$file = Get-Item -LiteralPath $VoiceFile
if ($file.Length -ne $expectedBytes) {
    throw "Unexpected voice data length: $($file.Length), expected $expectedBytes"
}

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.ReadTimeout = 100
$serial.WriteTimeout = 15000
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.Open()
$input = [System.IO.File]::OpenRead($file.FullName)
$buffer = [byte[]]::new(4096)
$received = [System.Text.StringBuilder]::new()

try {
    $serial.WriteLine("tts-upload $expectedBytes $expectedCrc")
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and
           $received.ToString() -notmatch 'TTS_UPLOAD_READY') {
        [void]$received.Append($serial.ReadExisting())
        Start-Sleep -Milliseconds 20
    }
    if ($received.ToString() -notmatch 'TTS_UPLOAD_READY') {
        throw "Device did not enter TTS upload mode: $received"
    }
    $sent = 0L
    while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $serial.Write($buffer, 0, $count)
        $sent += $count
        # USB Serial/JTAG has no hardware flow control; pace below measured SD throughput.
        Start-Sleep -Milliseconds 20
        [void]$received.Append($serial.ReadExisting())
        if (($sent % (256 * 1024)) -lt $buffer.Length) {
            Write-Output ("TTS_UPLOAD_PROGRESS {0:F1}%" -f (100.0 * $sent / $expectedBytes))
        }
    }
    # SerialPort.Write only confirms that bytes entered the host driver buffer.
    $deadline = [DateTime]::UtcNow.AddSeconds(180)
    while ([DateTime]::UtcNow -lt $deadline -and
           $received.ToString() -notmatch 'TTS_UPLOAD result=') {
        [void]$received.Append($serial.ReadExisting())
        Start-Sleep -Milliseconds 50
    }
    Write-Output $received.ToString()
    if ($received.ToString() -notmatch 'TTS_UPLOAD result=ESP_OK') {
        throw "Voice data installation failed"
    }
} finally {
    $input.Dispose()
    $serial.Dispose()
}
