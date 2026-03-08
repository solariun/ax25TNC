#!/bin/bash

# Configuration - Vero VR-N76 KISS over BLE
RADIO_UUID="F6BD1536-15E3-556D-0A5B-D4A102DB5A20"
READ_UUID="00000003-ba2a-46c9-ae49-01b0961f68bb"
WRITE_UUID="00000002-ba2a-46c9-ae49-01b0961f68bb"

echo "Iniciando ponte Serial <-> BLE para VR-N76..."
echo "Aguardando conexão..."

# O parâmetro -p cria um link simbólico fixo, facilitando no QtTermTCP
ble-serial -d "$RADIO_UUID" -r "$READ_UUID" -w "$WRITE_UUID" -p /tmp/ttyVRN76

