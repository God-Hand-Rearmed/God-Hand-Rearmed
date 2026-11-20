@echo off
rem Replays the test cases in cases\ (claude/test-mode.md): the game's logic at 30 FPS and at 60 FPS, headless, then
rem the comparison report. Double-click it. A case is replayed again whenever GHR.exe changed since its last
rem replay (a new build = every case again: the regression check), otherwise skipped. All reports end up in
rem cases\summary.txt. One case at a time: the game cannot run twice at once. The game runs in this window (no
rem window of its own to steal the focus); a replay that ends with an error is tried once more and, if it fails again,
rem the case is not stamped, so the next double-click replays it.
rem Each run also appends the SHA-256 of its CSV / RAM / snapshot files to cases\fingerprints.txt (one line per case
rem and run): identical lines across builds = the game ran exactly the same. RUNS replays every case that many times
rem per build (1 normally; 2 to check memory / determinism work: Replay-Cases-Twice.cmd). With 2, the whole-RAM snapshots of the previous build
rem (.snap.prev.bin) are compared with this build's, leaving out the words that differ between this build's own two
rem runs (tools\fps60\snap_compare.py; its verdicts go to fingerprints.txt too). Run 1's CSV stays as <case>.<rate>.csv.run1,
rem so a run whose CSV differs can be compared frame by frame.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not defined RUNS set RUNS=1
if not exist cases\*.case (
    echo No cases in %~dp0cases
    goto :end
)
for %%e in (GHR.exe) do set "BUILD=%%~te %%~ze"
for %%f in (cases\*.case) do (
    set "OLD="
    if exist "%%f.build" set /p OLD=<"%%f.build"
    if not "!OLD!"=="!BUILD!" (
        set "FAILED="
        for %%r in (30 60) do (
            if exist "%%f.%%r.snap.bin" move /y "%%f.%%r.snap.bin" "%%f.%%r.snap.prev.bin" >nul
            del /q "%%f.%%r.snap.run1.bin" "%%f.%%r.snapdiff.txt" "%%f.%%r.csv.run1" 2>nul
        )
        for /l %%n in (1,1,%RUNS%) do (
            del /q "%%f.30.csv" "%%f.60.csv" "%%f.30.shapes.csv" "%%f.60.shapes.csv" "%%f.report.txt" 2>nul
            del /q "%%f.30.ram.bin" "%%f.60.ram.bin" "%%f.30.snap.bin" "%%f.60.snap.bin" "%%f.30.rolls.csv" "%%f.60.rolls.csv" 2>nul
            for %%r in (0 1) do (
                if %%r==0 (set "RATE=30") else (set "RATE=60")
                echo Replaying %%~nxf at !RATE! FPS ^(run %%n of %RUNS%^)...
                GHR.exe --headless --replay-case "%%f" --fps60 %%r
                if not "!errorlevel!"=="0" (
                    echo   the !RATE! FPS replay stopped early ^(exit code !errorlevel!^); trying it again...
                    GHR.exe --headless --replay-case "%%f" --fps60 %%r
                    if not "!errorlevel!"=="0" set "FAILED=1"
                )
            )
            set "LINE=!BUILD!;run %%n;%%~nxf"
            for %%r in (30 60) do for %%k in (csv ram.bin snap.bin) do (
                set "H=missing"
                if exist "%%f.%%r.%%k" (
                    set "H="
                    for /f "skip=1 delims=" %%h in ('certutil -hashfile "%%f.%%r.%%k" SHA256') do if not defined H set "H=%%h"
                    if defined H (set "H=!H: =!") else (set "H=unreadable")
                )
                set "LINE=!LINE!;%%r.%%k=!H!"
            )
            >>cases\fingerprints.txt echo !LINE!
            if %%n==1 if %RUNS% GEQ 2 for %%r in (30 60) do (
                if exist "%%f.%%r.snap.bin" move /y "%%f.%%r.snap.bin" "%%f.%%r.snap.run1.bin" >nul
                if exist "%%f.%%r.csv" copy /y "%%f.%%r.csv" "%%f.%%r.csv.run1" >nul
            )
        )
        echo Comparing %%~nxf...
        python tools\fps60\case_report.py "%%f" > "%%f.report.txt" 2>&1
        if %RUNS% GEQ 2 python tools\fps60\snap_compare.py "%%f" >> cases\fingerprints.txt 2>&1
        if defined FAILED (
            echo   a replay failed twice: this case runs again next time
        ) else (
            >"%%f.build" echo !BUILD!
        )
    ) else (
        echo %%~nxf: already replayed with this build
    )
)
(for %%f in (cases\*.case) do (
    type "%%f.report.txt"
    echo.
    echo ------------------------------------------------------------------------------------------------------------
)) > cases\summary.txt 2>nul
findstr /c:"extension RAM in use" logs\*.log > cases\xram.txt 2>nul
echo Done. Reports: cases\summary.txt  Fingerprints: cases\fingerprints.txt
:end
if "%~1"=="" pause
