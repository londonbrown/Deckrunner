@echo off
rem Deckrunner's one-time setup: builds Deckrunner's data from your own Skate 3 disc and Cyberpunk 2077 install.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\setup.ps1" %*
