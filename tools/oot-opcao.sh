#!/bin/sh
# Troca opcoes do options.txt no aparelho pela rede (sem tirar o cartao). Roda NO WSL:
#   sh tools/oot-opcao.sh                              -> so mostra as opcoes atuais
#   sh tools/oot-opcao.sh stream_draw 0                -> troca uma
#   sh tools/oot-opcao.sh stream_draw 0 fused_draw 1   -> troca varias (uma senha so)
# Feche o jogo antes: ele regrava o arquivo quando abre (e le as opcoes so na abertura).
# (Versoes antigas do jogo usavam o opcoes.txt, com nomes em portugues: usado se o options.txt ainda nao existir.)
APARELHO=${APARELHO:-onion@192.168.253.23}
PASTA=/mnt/SDCARD/Roms/PORTS/Games/ZeldaOoT
CMD="cd $PASTA || exit 1; F=options.txt; [ -f \$F ] || [ ! -f opcoes.txt ] || F=opcoes.txt; touch \$F"
while [ $# -ge 2 ]; do
    # troca a linha; se a opcao ainda nao esta no arquivo (versao nova do jogo), acrescenta
    CMD="$CMD; if grep -q '^$1 = ' \$F; then sed -i 's/^$1 = .*/$1 = $2/' \$F; else printf '%s = %s\\n' '$1' '$2' >> \$F; fi"
    shift 2
done
CMD="$CMD; sync; echo \"(\$F)\"; grep -v '^#' \$F | grep ."
ssh "$APARELHO" "$CMD"
