@echo off
rem Test launch: GPU time by pass in the log, every 30th frame. Otherwise the same as Launch-Bayonetta3.cmd
set SNX_NVN_PASSTIME=30
call "%~dp0Launch-Bayonetta3.cmd"
