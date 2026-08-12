@echo off
setlocal
set "IDF_PATH=C:\Users\user\esp\v5.5.4\esp-idf"
set "IDF_PYTHON_ENV_PATH=C:\Users\user\.espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=C:\Users\user\.espressif\tools\esp-rom-elfs\20241011"
set "PATH=C:\Users\user\.espressif\tools\cmake\3.30.2\bin;C:\Users\user\.espressif\tools\ninja\1.12.1;C:\Users\user\.espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Users\user\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%PATH%"
set "CC=C:\Users\user\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe"
set "CXX=C:\Users\user\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-g++.exe"
set "ASM=C:\Users\user\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe"
"C:\Users\user\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" "C:\Users\user\esp\v5.5.4\esp-idf\tools\idf.py" -DCMAKE_MAKE_PROGRAM=C:\Users\user\.espressif\tools\ninja\1.12.1\ninja.exe %*
exit /b %errorlevel%
