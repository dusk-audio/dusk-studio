@echo off
rem usage: built.cmd <commit> <1 when ninja did no work, else 0>
rem After a successful build: fails when the tree last built another commit,
rem files the build reads changed between the two, and ninja still did no
rem work; otherwise records <commit> as built in stamps\built-commit.
setlocal
call "%~dp0env.cmd" || exit /b 1
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0built-check.ps1" -Root "%ROOT%" -Head "%~1" -NoWork "%~2"
exit /b %ERRORLEVEL%
