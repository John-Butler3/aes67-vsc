@echo off
rem Starts the AES67 bridge in the layout that Microsoft Teams / Zoom / browsers need.
rem Requires the free VB-CABLE virtual cable: https://vb-audio.com/Cable/  (install, reboot once).
setlocal
cd /d "%~dp0"
"%~dp0aes67vsc-bridge.exe" --list > "%TEMP%\aes67vsc-devices.txt" 2>&1
findstr /i /c:"CABLE Input" "%TEMP%\aes67vsc-devices.txt" >nul
if errorlevel 1 (
  echo VB-CABLE was not found. Install it from https://vb-audio.com/Cable/ ^(free^), reboot, and run this again.
  echo Devices seen on this PC:
  type "%TEMP%\aes67vsc-devices.txt"
  pause
  exit /b 1
)
rem With a second cable (VB-CABLE A, donationware) Teams can play into it, keeping other PC sounds out of the stream.
findstr /i /c:"CABLE-A Input" "%TEMP%\aes67vsc-devices.txt" >nul
if not errorlevel 1 (
  set "TXSRC=CABLE-A Input"
  set "TEAMS_SPK=CABLE-A Input (VB-Audio Cable A)"
) else (
  set "TXSRC=default"
  set "TEAMS_SPK=your normal speakers / headset (the default playback device)"
)
echo.
echo  AES67 bridge for Teams
echo  ----------------------
echo  In Teams ^> Settings ^> Devices set:
echo    Microphone : CABLE Output (VB-Audio Virtual Cable)   ^<- carries the AES67 input stream
echo    Speaker    : %TEAMS_SPK%   ^<- is sent as the AES67 output stream
echo  Tip: in Teams turn Noise suppression off ^(or on "Low"^) and enable High fidelity music mode
echo       if you are sending programme audio rather than speech.
echo  Keep this window open while you use Teams. Press Ctrl-C to stop.
echo.
"%~dp0aes67vsc-bridge.exe" --play "CABLE Input" --loopback "%TXSRC%"
pause
