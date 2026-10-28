@echo off
rem Unregisters the AES67 Virtual Sound Card ASIO driver. Asks for administrator rights itself.
setlocal
set "HERE=%~dp0"
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator rights...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
cd /d "%HERE%"
regsvr32 /s /u "%HERE%aes67asio.dll"
netsh advfirewall firewall delete rule name="AES67 VSC" >nul 2>&1
echo Driver unregistered. You can delete this folder now. Settings remain in %APPDATA%\AES67VSC.
pause
