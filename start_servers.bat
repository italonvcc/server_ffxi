@echo off
rem Starts each LandSandBoat server in its own console window.
cd /d "%~dp0"
start "xi_connect" xi_connect.exe
start "xi_search" xi_search.exe
start "xi_profile" xi_profile.exe
start "xi_world" xi_world.exe
start "xi_map" xi_map.exe
