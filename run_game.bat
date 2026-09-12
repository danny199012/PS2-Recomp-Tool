@echo off
cd /d E:\PS2-Recomp-Tool
ee-runner.exe "E:\Aura\PS2 Recomp Research\game\Exports\SLUS_210.66" --import "E:\Aura\PS2 Recomp Research\game\Exports\functions.csv" --config "E:\Aura\PS2 Recomp Research\game\Exports\config.toml" --iso "E:\Aura\PS2 Recomp Research\game\Exports\Urbz, The - Sims in the City (USA) [SLUS-21066].iso" > game_log.txt 2>&1
echo EXIT=%ERRORLEVEL%
