#!/bin/sh
# Traz os quadros lentos gravados no aparelho (lento_NN.cap e .png, opcao captura_lenta) para
# a pasta oot_lentos no Desktop do Windows. Roda NO WSL.
APARELHO=${APARELHO:-onion@192.168.253.23}
DEST="/mnt/c/Users/Régis/Desktop/oot_lentos"
mkdir -p "$DEST" || exit 1
scp "$APARELHO:/mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT/lento_*" "$DEST/" &&
    ls -la "$DEST"
