call SignAndLoad_FSBL.bat %1 < nul
call SignAndLoad_App.bat %1 < nul
call LoadAssets.bat %1 < nul
pause
