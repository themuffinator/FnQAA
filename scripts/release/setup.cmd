@echo off
rem FnQAA setup (Windows). Finds your Quake Arena Arcade package in this
rem folder (the XBLA zip, the zip extracted, your Xbox 360 Content folder or
rem the package file), recompiles QAA's game code from it on this PC and
rem installs the result here. Run it again after updating FnQAA.
rem Options are passed to setup\tools\recomp\build.py (--help lists them).
setlocal
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY (
	echo Python 3 is needed: winget install Python.Python.3.13
	goto :end
)
%PY% "%~dp0setup\tools\recomp\build.py" %*
:end
set "RC=%ERRORLEVEL%"
rem keep the window open when started by double-click
echo %CMDCMDLINE% | findstr /i /c:"%~nx0" >nul && pause
exit /b %RC%
