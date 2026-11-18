@echo off
rem Deckrunner's one-time setup: builds Deckrunner's data from your own Skate 3 disc and Cyberpunk 2077 install.
rem It runs setup\setup.py on Python 3: the private copy in .\python if setup made one, else yours (py, then python).
rem With no Python at all it asks, then downloads python.org's embeddable Python (checked against its SHA-256), and
rem setup.py adds numpy and Pillow. No PowerShell: antivirus heuristics flag script hosts run from a download folder.
setlocal
title Deckrunner setup
cd /d "%~dp0"
if not exist "setup\setup.py" goto notextracted
if exist "python\python.exe" goto private
py -3 -B -s setup\setup.py --probe >nul 2>&1
if not errorlevel 1 goto py
python -B -s setup\setup.py --probe >nul 2>&1
if not errorlevel 1 goto python
goto nopython

:py
py -3 -B -s setup\setup.py %*
goto end

:python
python -B -s setup\setup.py %*
goto end

:nopython
echo.
echo   Setup needs Python 3.11 or newer with numpy and Pillow, and none was found.
echo   It can download a private copy into the folder "python" here: about 31 MB from python.org and PyPI,
echo   each file checked against its published SHA-256. Nothing is installed system-wide.
echo.
choice /c YN /n /m "  Download it now? [Y/N] "
if errorlevel 2 goto declined
if exist python.download rmdir /s /q python.download
mkdir python.download\py || goto failed
echo == python: Downloading python-3.13.15-embed-amd64.zip
curl.exe -fL --retry 2 -o python.download\python.zip https://www.python.org/ftp/python/3.13.15/python-3.13.15-embed-amd64.zip || goto failed
set "SHA="
for /f "skip=1 delims=" %%h in ('certutil -hashfile python.download\python.zip SHA256') do if not defined SHA set "SHA=%%h"
if not defined SHA goto badhash
set "SHA=%SHA: =%"
if /i not "%SHA%"=="d1f04d990aee1253d8569e8e5104e30fa9f5fa830899f14843448872d936a2cf" goto badhash
tar -xf python.download\python.zip -C python.download\py || goto failed
del /q python.download\py\python*._pth
if exist python rmdir /s /q python
move python.download\py python >nul || goto failed
rmdir /s /q python.download

:private
set "PYTHONHOME=%CD%\python"
set "PYTHONPATH="
set "PYTHONNOUSERSITE=1"
python\python.exe -B -s setup\setup.py %*
goto end

:notextracted
echo.
echo   Extract the whole Deckrunner zip into a folder first, then run "Deckrunner Setup.cmd" from there.
goto failed

:badhash
echo.
echo   python-3.13.15-embed-amd64.zip does not match its published SHA-256, so nothing was installed.
goto failed

:declined
echo.
echo   Setup needs Python. Install Python 3.11+ with numpy and Pillow, or run setup again and let it download one.

:failed
echo.
pause

:end
endlocal
