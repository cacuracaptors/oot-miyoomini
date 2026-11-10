#!/bin/sh
# Monta a pasta do jogo para o Miyoo Mini Plus (OnionOS), a partir do build ARM:
#   sh oot-package.sh
#
# Resultado: $ROOT/oot-arm/pacote/ com a mesma estrutura do cartao SD:
#   Roms/PORTS/Games/ZeldaOoT/        oot (executavel), oot_swapmap.bin, saves/
#   Roms/PORTS/Shortcuts/Zelda - Ocarina of Time.port
# Nada de dados do jogo: o cartucho (oot.cart) vai separado (ou a ROM, e o jogo cria o
# cartucho na primeira vez que abrir, no proprio aparelho).
ROOT=${ROOT:-$HOME/fallout-miyoo}
OUT=${OUT:-$ROOT/oot-arm}
TC=${TC:-$ROOT/miyoomini-toolchain/bin/arm-linux-gnueabihf}
P=${P:-$OUT/pacote}
GAME="$P/Roms/PORTS/Games/ZeldaOoT"
# para o release (tools/oot-release.sh): EXE = o executavel publicado, RCP = a receita dos dados do jogo,
# PAK = texturas_hd.pak (so a logo), IMG = a imagem do atalho
EXE=${EXE:-$OUT/oot_headless}

[ -f "$EXE" ] || { echo "ERRO: falta $EXE (rode antes: sh tools/oot-build.sh)"; exit 1; }
[ "$(cat "$OUT/link.status" 2>/dev/null)" = 0 ] || { echo "ERRO: a ultima ligacao falhou (veja $OUT/link.log)"; exit 1; }
case "$(file -b "$EXE" 2>/dev/null)" in
    *ARM*) ;;
    *) echo "ERRO: $EXE nao e um executavel ARM"; exit 1 ;;
esac

MAP=""
for m in "$OUT/oot_swapmap.bin" "$ROOT/oot-i386/oot_swapmap.bin"; do
    if [ -f "$m" ]; then
        MAP=$m
        break
    fi
done
[ -n "$MAP" ] || { echo "ERRO: nao achei oot_swapmap.bin (em $OUT nem em $ROOT/oot-i386)"; exit 1; }
if [ -f "$ROOT/oot-i386/oot_swapmap.bin" ] && ! cmp -s "$MAP" "$ROOT/oot-i386/oot_swapmap.bin"; then
    echo "AVISO: $MAP e diferente do mapa de $ROOT/oot-i386: o oot.cart feito no PC nao vale no aparelho"
    echo "       (o jogo vai pedir a ROM para refazer o cartucho)."
fi

rm -rf "$P"
mkdir -p "$GAME/saves" "$P/Roms/PORTS/Shortcuts" || exit 1
cp "$EXE" "$GAME/oot" && chmod 755 "$GAME/oot" || exit 1
if [ -n "$RCP" ]; then
    cp "$RCP" "$GAME/oot_dados.rcp" || exit 1
fi
if [ -n "$PAK" ]; then
    cp "$PAK" "$GAME/texturas_hd.pak" || exit 1
fi
if [ -n "$IMG" ]; then
    mkdir -p "$P/Roms/PORTS/Imgs" && cp "$IMG" "$P/Roms/PORTS/Imgs/Zelda - Ocarina of Time.png" || exit 1
fi
cp "$MAP" "$GAME/oot_swapmap.bin" || exit 1
# pacotes de idioma (opcao idioma no opcoes.txt)
PORTDIR="$(cd "$(dirname "$0")/.." && pwd)"
# leia-me do jogador (em ingles), na pasta do jogo
cp "$PORTDIR/dist/README.txt" "$GAME/README.txt" || exit 1
# atualizacoes (port/miyoo/update_miyoo.c): a versao instalada e a lista de certificados para conferir o HTTPS do GitHub
cp "$PORTDIR/dist/cacert.pem" "$GAME/cacert.pem" || exit 1
if [ -n "$VERSION" ]; then
    echo "$VERSION" > "$GAME/version.txt" || exit 1
fi
if [ -d "$PORTDIR/idiomas" ]; then
    mkdir -p "$GAME/idiomas" && cp "$PORTDIR"/idiomas/*.oot "$GAME/idiomas/" || exit 1
fi

cat > "$P/Roms/PORTS/Shortcuts/Zelda - Ocarina of Time.port" <<'PORT'
#!/bin/sh
# clear the screen first (the Game Switcher's picture of the last scene stays in the screen memory)
cat /dev/zero > /dev/fb0 2>/dev/null
GameName="Zelda - Ocarina of Time"
GameDir="ZeldaOoT"
GameExecutable="oot"
GameDataFile="oot"

KillAudioserver=1
PerformanceMode=1

FullGamePath="/mnt/SDCARD/Roms/PORTS/Games/$GameDir"
GameFolder="$FullGamePath"
export HOME="$FullGamePath"

# Options (add them to Arguments, separated by spaces):
#   -perf     timing lines in log.txt every 2 s (to measure the performance)
#   -mudo     no sound
#   -semgfx   copy to the screen with the CPU (without the chip's 2D engine)
Arguments=""

cd "$GameFolder"
/mnt/SDCARD/Emu/PORTS/launch_standalone.sh "$GameName" "$GameDir" "$GameExecutable" "$Arguments" "$GameDataFile" "$KillAudioserver" "$PerformanceMode" > "$GameFolder/log.txt" 2>&1
PORT
chmod 755 "$P/Roms/PORTS/Shortcuts/Zelda - Ocarina of Time.port"

echo "pacote: $P"
ls -l "$GAME" "$P/Roms/PORTS/Shortcuts"
find "$GAME" -maxdepth 1 -type f -exec md5sum {} + # (so arquivos: md5sum numa pasta da erro e parava o release)
