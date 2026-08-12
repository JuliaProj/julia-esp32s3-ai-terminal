@echo off
setlocal
if "%~1"=="" (
  echo Usage: flash_windows.bat COM5
  exit /b 2
)
set PORT=%~1
where esptool.exe >nul 2>nul
if errorlevel 1 (
  echo esptool.exe not found. Install ESP-IDF 5.5.x or add esptool to PATH.
  exit /b 3
)
esptool.exe --chip esp32s3 -p %PORT% -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 bootloader.bin 0x20000 julia-ui.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x920000 srmodels.bin
endlocal
