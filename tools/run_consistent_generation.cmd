@echo off
cd /d D:\Julia_Bot\esp_proj
start "Julia consistent asset generation" /b "C:\Users\user\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" -u tools\generate_all_animations.py --output assets\state_assets_consistent_v2 1>assets\state_assets_consistent_v2\generation.log 2>assets\state_assets_consistent_v2\generation.err.log
