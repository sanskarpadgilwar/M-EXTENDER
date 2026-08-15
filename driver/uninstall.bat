@echo off
setlocal
rem Remove the TwinScreen virtual display driver.
rem Run as Administrator. See README.md for the boot-loop recovery path.
cd /d "%~dp0"

echo Installed driver packages:
echo.
pnputil /enum-drivers | findstr /i /c:"TwinScreen" /c:"oem"
echo.
echo Find the "oem##.inf" that belongs to TwinScreen above, then run:
echo   pnputil /delete-driver oem##.inf /uninstall
echo.
echo (pnputil /enum-drivers shows every package; look for a publisher of
echo  "TwinScreen" / the TwinScreen Virtual Display device.)
endlocal
