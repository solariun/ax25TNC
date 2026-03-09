#!/bin/bash

# --- Configuration & Constants (No Magic Numbers) ---
DEFAULT_RADIO_NAME="38:D2:00:01:61:35"
RADIO_NAME=${1:-$DEFAULT_RADIO_NAME}
MY_CALL="MYCALL-1"                   # IMPORTANT: Update in /etc/ax25/axports
KISS_PORT="/tmp/ttyKISS"
AX_PORT_FILE="/etc/ax25/axports"
BIN_DIR="/usr/local/bin"

# pipx Global Path Overrides
GLOBAL_BIN_DIR="/usr/local/bin"
GLOBAL_HOME_DIR="/opt/pipx"

export PIPX_HOME="$GLOBAL_HOME_DIR"
export PIPX_BIN_DIR="$GLOBAL_BIN_DIR"

# Ensure script is run as root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (sudo)"
   exit 1
fi

echo "--- [1/6] Installing System Dependencies ---"
apt-get update # && apt-get upgrade -y
apt-get install -y build-essential git bluez libax25-dev ax25-tools ax25-apps \
                   python3-pip python3-venv pipx socat git cmake screen \
                   libncurses5-dev zlib1g-dev libasound2-dev \
                   direwolf linpac


echo "\t * unblocking the bluetooth to activate it"
sudo rfkill unblock bluetooth

echo "--- [2/6] Installing LinBPQ ---"
mkdir -p /opt/linbpq && cd /opt/linbpq
wget http://www.cantab.net/users/john.wiseman/Downloads/Beta/linbpq
chmod +x linbpq

echo "--- [3/6] Installing ble-serial via pipx ---"
pipx install ble-serial
pipx ensurepath
export PATH="$PATH:/root/.local/bin"

echo "--- [4/6] Configuring AX.25 Ports ---"
if ! grep -q "vhf" "$AX_PORT_FILE"; then
    echo "vhf    $MY_CALL    1200    128     3       VHF 2m Packet" >> "$AX_PORT_FILE"
    echo "hf     ${MY_CALL%-*}-2    300     64      2       HF 30m Packet" >> "$AX_PORT_FILE"
fi

echo "--- [5/6] Creating Radio Connection Logic Script ---"
cat << EOF > "$BIN_DIR/radio_connect.sh"
#!/bin/bash
# Script to bridge BLE to KISS Serial for BBS-Modem
TARGET_NAME="$1"
TIMEOUT=15
PORT="/tmp/ttyKISS"
BLE_SCAN="/root/.local/bin/ble-scan"
BLE_SERIAL="/root/.local/bin/ble-serial"

echo "Scanning for $TARGET_NAME..."
#ID=$($BLE_SCAN -t "${TIMEOUT}" | grep -i "$TARGET_NAME" | awk '{print $1}' | head -n 1 | tr -d '[:cntrl:]')
ID="$TARGET_NAME"

if [ -z "$ID" ]; then 
    echo "Device $TARGET_NAME not found. Retrying..."; exit 1
fi

echo "Performing Deep Scan on $ID..."
SCAN=$($BLE_SCAN -t "${TIMEOUT}" -d "$ID")
# Standard GATT handles for VR-N76 series serial data
R_UUID=$(echo "$SCAN" | grep "00000003" | awk '{print $2}')
W_UUID=$(echo "$SCAN" | grep "00000002" | awk '{print $2}')

if [ -z "$R_UUID" ] || [ -z "$W_UUID" ]; then
    echo "Could not resolve GATT characteristics for KISS."; exit 1
fi

echo "Connecting to $ID (Address: Public) for KISS operation..."
exec $BLE_SERIAL -v -t "${TIMEOUT}" -a public -d "$ID" -r "$R_UUID" -w "$W_UUID" -p "$PORT"
EOF

chmod +x "$BIN_DIR/radio_connect.sh"

echo "--- [6/6] Creating Modular Systemd Service Chain ---"

# Service 1: The BLE Serial Bridge (Renamed to radio_connect)
cat << EOF > /etc/systemd/system/radio_connect.service
[Unit]
Description=Radio BLE to Serial Bridge ($RADIO_NAME)
After=bluetooth.target
[Service]
ExecStart=$BIN_DIR/radio_connect.sh "$RADIO_NAME"
Restart=always
RestartSec=15
ExecStopPost=/bin/rm -f $KISS_PORT
[Install]
WantedBy=multi-user.target
EOF

# Service 2: The KISS Attachment (Depends on radio_connect)
cat << EOF > /etc/systemd/system/kiss-attach.service
[Unit]
Description=AX.25 KISS Attach
Requires=radio_connect.service
After=radio_connect.service
BindsTo=radio_connect.service
[Service]
Type=forking
ExecStartPre=/bin/sh -c 'while [ ! -e $KISS_PORT ]; do sleep 1; done'
ExecStart=/usr/sbin/kissattach $KISS_PORT vhf
ExecStartPost=/usr/sbin/kissparms -p vhf -t 100 -s 100 -r 25 -l 128 -v 3
ExecStop=/bin/ifconfig vhf down
Restart=on-failure
RestartSec=10
EOF

# Service 3: LinPac Application (Depends on KISS)
cat << EOF > /etc/systemd/system/linpac.service
[Unit]
Description=LinPac Terminal over Screen
Requires=kiss-attach.service
After=kiss-attach.service
BindsTo=kiss-attach.service
[Service]
Type=forking
ExecStart=/usr/bin/screen -d -m -S linpac /usr/bin/linpac
ExecStop=/usr/bin/screen -S linpac -X quit
Restart=on-failure
RestartSec=20
[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable linpac.service

echo "--------------------------------------------------------"
echo "INSTALLATION COMPLETE"
echo "Radio Target: $RADIO_NAME"
echo "Service Name: radio_connect.service"
echo "--------------------------------------------------------"
echo "1. Verify/Edit your callsign in: $AX_PORT_FILE"
echo "2. Start the stack: sudo systemctl start linpac.service"
echo "3. View LinPac: screen -r linpac"
echo "4. Monitor logs: journalctl -u radio_connect -f"
echo "--------------------------------------------------------"
