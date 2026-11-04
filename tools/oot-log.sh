#!/bin/sh
# Traz o log.txt do aparelho para o Desktop do Windows (oot_log_miyoo.txt). Roda NO WSL.
APARELHO=${APARELHO:-onion@192.168.253.23}
scp "$APARELHO:/mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT/log.txt" "/mnt/c/Users/Régis/Desktop/oot_log_miyoo.txt" &&
    echo "log: /mnt/c/Users/Régis/Desktop/oot_log_miyoo.txt"
