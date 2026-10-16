@echo off
rem 双击运行:一键获取拓竹 accessToken(详见 get-token.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get-token.ps1"
pause
