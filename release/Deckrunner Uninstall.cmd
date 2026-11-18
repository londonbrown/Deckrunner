@echo off
rem Removes Deckrunner (its plugin, data and two archives) from your Cyberpunk 2077 folder. Your deckrunner.ini stays.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\setup.ps1" -Uninstall
