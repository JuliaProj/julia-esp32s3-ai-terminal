param(
    [string]$Port = "COM5",
    [int]$BaudRate = 115200,
    [string]$OutputRoot = "tmp/ota-hardware-tests",
    [int]$ResponseTimeoutMs = 1500,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$cases = @(
    [PSCustomObject]@{
        Id = 'HW-OTA-001'
        Name = 'Initial state'
        Commands = @('ota mock reset', 'ota status')
        Expected = 'OTA status=0 type=0 progress=0 error=0 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-002'
        Name = 'Firmware success'
        Commands = @(
            'ota mock reset',
            'ota mock start firmware 0 0 v-test start',
            'ota mock progress firmware 25 0 v-test downloading',
            'ota mock progress firmware 60 0 v-test downloading',
            'ota mock progress firmware 100 0 v-test downloaded',
            'ota mock verify firmware 100 0 v-test verifying',
            'ota mock install firmware 100 0 v-test installing',
            'ota mock complete firmware 100 0 v-test complete',
            'ota status'
        )
        Expected = 'OTA status=4 type=0 progress=100 error=0 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-003'
        Name = 'Asset success'
        Commands = @(
            'ota mock reset',
            'ota mock start asset 0 0 asset-test start',
            'ota mock progress asset 50 0 asset-test downloading',
            'ota mock progress asset 100 0 asset-test downloaded',
            'ota mock verify asset 100 0 asset-test verifying',
            'ota mock install asset 100 0 asset-test installing',
            'ota mock complete asset 100 0 asset-test complete',
            'ota status'
        )
        Expected = 'OTA status=4 type=1 progress=100 error=0 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-004'
        Name = 'Download failure'
        Commands = @(
            'ota mock reset',
            'ota mock start firmware 0 0 v-test start',
            'ota mock progress firmware 40 0 v-test downloading',
            'ota mock fail firmware 40 1 v-test download_failed',
            'ota status'
        )
        Expected = 'OTA status=5 type=0 progress=40 error=1 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-005'
        Name = 'Verification failure'
        Commands = @(
            'ota mock reset',
            'ota mock start firmware 0 0 v-test start',
            'ota mock progress firmware 100 0 v-test downloaded',
            'ota mock verify firmware 100 0 v-test verifying',
            'ota mock fail firmware 100 2 v-test verify_failed',
            'ota status'
        )
        Expected = 'OTA status=5 type=0 progress=100 error=2 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-006'
        Name = 'Install failure'
        Commands = @(
            'ota mock reset',
            'ota mock start asset 0 0 asset-test start',
            'ota mock progress asset 100 0 asset-test downloaded',
            'ota mock verify asset 100 0 asset-test verifying',
            'ota mock install asset 100 0 asset-test installing',
            'ota mock fail asset 100 3 asset-test install_failed',
            'ota status'
        )
        Expected = 'OTA status=5 type=1 progress=100 error=3 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-007'
        Name = 'Rollback failure'
        Commands = @(
            'ota mock reset',
            'ota mock start asset 0 0 asset-test start',
            'ota mock rollback asset 0 4 asset-test rollback_failed',
            'ota status'
        )
        Expected = 'OTA status=5 type=1 progress=0 error=4 active=0'
    },
    [PSCustomObject]@{
        Id = 'HW-OTA-008'
        Name = 'Invalid arguments'
        Commands = @(
            'ota mock reset',
            'ota mock progress firmware 101 0 v-test invalid',
            'ota status'
        )
        Expected = 'OTA status=0 type=0 progress=0 error=0 active=0'
    }
)

if ($DryRun) {
    foreach ($case in $cases) {
        Write-Output "[$($case.Id)] $($case.Name)"
        $case.Commands | ForEach-Object { Write-Output "  $_" }
        Write-Output "  EXPECT: $($case.Expected)"
    }
    exit 0
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$outputDirectory = Join-Path $OutputRoot $stamp
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$logPath = Join-Path $outputDirectory 'serial.log'
$reportPath = Join-Path $outputDirectory 'OTA_TEST_REPORT.md'
$serialLog = [System.Text.StringBuilder]::new()
$results = [System.Collections.Generic.List[object]]::new()

function Read-SerialWindow {
    param([System.IO.Ports.SerialPort]$Serial, [int]$TimeoutMs)
    $response = [System.Text.StringBuilder]::new()
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = $Serial.ReadExisting()
        if ($chunk.Length -gt 0) {
            [void]$response.Append($chunk)
            [void]$serialLog.Append($chunk)
        }
        Start-Sleep -Milliseconds 20
    }
    return $response.ToString()
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, 'None', 8, 'One')
$serial.WriteTimeout = 3000
$serial.ReadTimeout = 50
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.Open()

try {
    Start-Sleep -Milliseconds 500
    [void](Read-SerialWindow -Serial $serial -TimeoutMs 200)

    foreach ($case in $cases) {
        $caseText = [System.Text.StringBuilder]::new()
        $ackFailure = $false
        foreach ($command in $case.Commands) {
            [void]$serialLog.AppendLine("COMMAND $command")
            $serial.WriteLine($command)
            $response = Read-SerialWindow -Serial $serial -TimeoutMs $ResponseTimeoutMs
            [void]$caseText.Append($response)
            if ($command -like 'ota mock *' -and $command -notlike '* 101 *') {
                if ($response -notmatch 'OTA_MOCK .*result=ESP_OK') {
                    $ackFailure = $true
                }
            }
            if ($command -like '* 101 *' -and $response -notmatch 'Usage: ota mock') {
                $ackFailure = $true
            }
        }

        $text = $caseText.ToString()
        $statusMatches = [regex]::Matches($text, 'OTA status=[^\r\n]+')
        $lastStatus = if ($statusMatches.Count) {
            $statusMatches[$statusMatches.Count - 1].Value
        } else {
            ''
        }
        $statusPass = $lastStatus.Contains($case.Expected)
        $crash = $text -match 'Guru Meditation|Task watchdog|panic|rst:0x'
        $passed = !$ackFailure -and $statusPass -and !$crash
        $results.Add([PSCustomObject]@{
            Id = $case.Id
            Name = $case.Name
            Passed = $passed
            AckFailure = $ackFailure
            Crash = $crash
            Expected = $case.Expected
            Actual = $lastStatus
        })
        Write-Output ("{0} {1}: {2}" -f $case.Id, $case.Name,
                      $(if ($passed) { 'PASS' } else { 'FAIL' }))
    }
} finally {
    $serial.Close()
}

[System.IO.File]::WriteAllText($logPath, $serialLog.ToString(),
                               [System.Text.UTF8Encoding]::new($false))

$report = [System.Text.StringBuilder]::new()
[void]$report.AppendLine('# Julia OTA Hardware Test Report')
[void]$report.AppendLine('')
[void]$report.AppendLine("- Time: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
[void]$report.AppendLine("- Port: $Port")
[void]$report.AppendLine("- Baud: $BaudRate")
[void]$report.AppendLine('- Serial log: serial.log')
[void]$report.AppendLine('')
[void]$report.AppendLine('| Case | Result | Expected | Actual |')
[void]$report.AppendLine('|---|---|---|---|')
foreach ($result in $results) {
    $resultText = if ($result.Passed) { 'PASS' } else { 'FAIL' }
    $actual = $result.Actual.Replace('|', '\|')
    [void]$report.AppendLine("| $($result.Id) $($result.Name) | $resultText | " +
                            "$($result.Expected) | $actual |")
}
[void]$report.AppendLine('')
[void]$report.AppendLine('## Manual UI Checks')
[void]$report.AppendLine('')
[void]$report.AppendLine('| Check | Result | Notes |')
[void]$report.AppendLine('|---|---|---|')
[void]$report.AppendLine('| Upgrade lock screen | NOT INTEGRATED / PASS / FAIL | |')
[void]$report.AppendLine('| Progress updates | NOT INTEGRATED / PASS / FAIL | |')
[void]$report.AppendLine('| Verify/install states | NOT INTEGRATED / PASS / FAIL | |')
[void]$report.AppendLine('| Error and retry UI | NOT INTEGRATED / PASS / FAIL | |')
[void]$report.AppendLine('| No flicker or animation stall | NOT RUN / PASS / FAIL | |')
[void]$report.AppendLine('')
[void]$report.AppendLine('Note: the current firmware has no OTA UI event subscriber. Interface PASS does not imply UI PASS.')
[System.IO.File]::WriteAllText($reportPath, $report.ToString(),
                               [System.Text.UTF8Encoding]::new($false))

$failed = @($results | Where-Object { !$_.Passed }).Count
Write-Output "OTA_TEST_REPORT path=$reportPath passed=$($results.Count - $failed) failed=$failed"
if ($failed -gt 0) { exit 1 }
