@echo off
rem Registers the AES67 Virtual Sound Card ASIO driver (64-bit). Run as administrator.
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo This installer needs administrator rights. Right-click install.bat and choose "Run as administrator".
  pause
  exit /b 1
)
if not exist "%~dp0aes67asio.dll" (
  echo aes67asio.dll not found next to this script.
  pause
  exit /b 1
)
echo Registering %~dp0aes67asio.dll ...
regsvr32 /s "%~dp0aes67asio.dll"
if %errorlevel% neq 0 (
  echo Registration failed.
  pause
  exit /b 1
)
echo Adding Windows Firewall rules for the driver host and control panel (UDP in)...
netsh advfirewall firewall delete rule name="AES67 VSC" >nul 2>&1
netsh advfirewall firewall add rule name="AES67 VSC" dir=in action=allow protocol=UDP localport=319,320,5004,9875 >nul
echo.
echo Done. "AES67 Virtual Sound Card" is now available as an ASIO device.
echo Keep this folder in place; the driver runs from here. Use aes67vsc-panel.exe to configure it.
pause
