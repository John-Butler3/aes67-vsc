@echo off
rem Unregisters the AES67 Virtual Sound Card ASIO driver. Run as administrator.
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo This needs administrator rights. Right-click uninstall.bat and choose "Run as administrator".
  pause
  exit /b 1
)
regsvr32 /s /u "%~dp0aes67asio.dll"
netsh advfirewall firewall delete rule name="AES67 VSC" >nul 2>&1
echo Driver unregistered. You can delete this folder now. Settings remain in %APPDATA%\AES67VSC.
pause
