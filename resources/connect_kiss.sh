#!/bin/bash

# =================================================================
# SCRIPT: Conectar VR-N76 via BLE-Serial
# USO: ./conectar-ble.sh "VR-N76"
# =================================================================

TARGET_NAME="$1"
TIMEOUT=10
TCP_PORT=8001  # Standard AGWPE/KISS port for many apps

if [ -z "$TARGET_NAME" ]; then
    echo "Erro: Forneça o nome do dispositivo ou parte dele. Ex: ./conectar-ble.sh VR-N76"
    exit 1
fi

echo "--- Passo 1: Localizando UUID do dispositivo: $TARGET_NAME ---"
# Busca o UUID baseado no nome amigável (padrão macOS)
DEVICE_UUID=$(ble-scan -t "${TIMEOUT}" | grep -i "$TARGET_NAME" | awk '{print $1}')
echo "$DEVICE_UUID"

if [ -z "$DEVICE_UUID" ]; then
    echo "Erro: Dispositivo '$TARGET_NAME' não encontrado no scan inicial."
    exit 1
fi

echo "Encontrado: $TARGET_NAME com UUID: $DEVICE_UUID"

echo "--- Passo 2: Fazendo Deep Scan para extrair Canais de Dados ---"
# Faz o deep scan e extrai os UUIDs de leitura e escrita automaticamente
# O VR-N76 usa a característica 00000003 para leitura e 00000002 para escrita
DEEP_SCAN=$(ble-scan -t "${TIMEOUT}" -d "$DEVICE_UUID")

# Lógica de extração baseada no seu dump anterior
READ_UUID=$(echo "$DEEP_SCAN" | grep "00000003" | awk '{print $2}')
WRITE_UUID=$(echo "$DEEP_SCAN" | grep "00000002" | awk '{print $2}')

if [ -z "$READ_UUID" ] || [ -z "$WRITE_UUID" ]; then
    echo "Erro: Não foi possível mapear as características de r/w automaticamente."
    exit 1
fi

echo "UUID Leitura (Notify): $READ_UUID"
echo "UUID Escrita (Write):  $WRITE_UUID"

echo "--- Passo 3: Iniciando ponte BLE-Serial ---"
echo "A porta será criada em /tmp/ttyKISS"

# Executa o ble-serial
# -p cria um link fixo para facilitar a configuração no QtTermTCP
ble-serial -v -a public -t "${TIMEOUT}" -d "$DEVICE_UUID" -r "$READ_UUID" -w "$WRITE_UUID"  --expose-tcp-port "${TCP_PORT}"

