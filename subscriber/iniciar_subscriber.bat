@echo off
title Subscriber MQTT - Controle de Acesso
cd /d "%~dp0"

echo ========================================================
echo Iniciando Subscriber MQTT (Controle de Acesso)
echo ========================================================

python -m pip install -r requirements.txt --quiet
python -u subscriber.py

pause
