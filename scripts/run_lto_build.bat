@echo off
chcp 65001 >nul
cd /d "J:\XBOX360 port\Transformers Dark of the Moon"
"C:\Users\Administrator\AppData\Roaming\qianfan-desktop-app\lightsandbox\git\bin\bash.exe" -c "bash scripts/build_switch.sh -j 6 > recomp/out/sw8/build_lto_diag2.log 2>&1"