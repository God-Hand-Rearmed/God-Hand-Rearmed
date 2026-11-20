@echo off
rem Replays every test case twice with this build, even the ones Replay-Cases.cmd already replayed with it, to check
rem that a replay repeats exactly (claude/test-mode.md). Double-click it. In cases\fingerprints.txt each case then has a
rem "run 1" and a "run 2" line that should match, and a snapshot line: "N words that change on their own" is how many
rem RAM words differed between the two runs (a few is normal), "DIFFERS from the previous build" compares with the
rem replay before this one.
setlocal
cd /d "%~dp0"
del /q cases\*.case.build 2>nul
set RUNS=2
call "%~dp0Replay-Cases.cmd" twice
pause
