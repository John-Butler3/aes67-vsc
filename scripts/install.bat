@echo off
rem Registers the AES67 Virtual Sound Card ASIO driver (64-bit). Asks for administrator rights itself.
setlocal
set "HERE=%~dp0"
echo AES67 Virtual Sound Card - installer
echo Folder: %HERE%
echo.

rem --- Opened from inside the zip? Then only this script was extracted, into the temp folder.
call set "T=%%HERE:%TEMP%=%%"
if /i not "%T%"=="%HERE%" (
  echo It looks like you opened the zip without extracting it.
  echo Extract the WHOLE zip to a folder on your local drive, for example C:\AES67VSC,
  echo then run install.bat from that folder.
  pause
  exit /b 1
)

rem --- Network location? The driver must be on a local disk.
if "%HERE:~0,2%"=="\\" goto network
fsutil fsinfo drivetype %~d0 2>nul | find /i "Remote" >nul && goto network

if not exist "%HERE%aes67asio.dll" (
  echo aes67asio.dll was not found in this folder. Files here:
  dir /b "%HERE%"
  echo.
  echo Extract the complete zip ^(all files^) to a local folder and run install.bat from there.
  pause
  exit /b 1
)

rem --- Elevate if needed.
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator rights...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)

cd /d "%HERE%"
echo Registering "%HERE%aes67asio.dll" ...
regsvr32 /s "%HERE%aes67asio.dll"
if %errorlevel% neq 0 (
  echo Registration failed ^(regsvr32 error %errorlevel%^).
  echo If Windows Defender or SmartScreen blocked the file, right-click aes67asio.dll, Properties, tick Unblock.
  pause
  exit /b 1
)
echo Adding a Windows Firewall rule for UDP 319,320,5004,9875 ...
netsh advfirewall firewall delete rule name="AES67 VSC" >nul 2>&1
netsh advfirewall firewall add rule name="AES67 VSC" dir=in action=allow protocol=UDP localport=319,320,5004,9875 >nul
echo.
echo Done. "AES67 Virtual Sound Card" is now available as an ASIO device.
echo Keep this folder where it is; the driver runs from here. Use aes67vsc-panel.exe to configure it.
pause
exit /b 0

:network
echo This folder is on a network location. Windows cannot load the driver from there
echo ^(and an elevated installer cannot see mapped network drives^).
echo Copy the whole folder to your local drive, for example C:\AES67VSC, and run install.bat there.
pause
exit /b 1
