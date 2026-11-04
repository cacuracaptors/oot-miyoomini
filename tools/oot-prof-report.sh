#!/bin/sh
# Transforma as amostras do "oot_replay --prof" em tabelas de tempo por funcao e por linha.
# Roda NO WSL:
#   sh ~/fallout-miyoo/oot-miyoo/tools/oot-prof-report.sh AMOSTRAS.txt EXECUTAVEL [SAIDA.txt]
#
# EXECUTAVEL tem de ser exatamente o binario que gerou as amostras (oot_replay_arm para o Miyoo,
# oot_replay para o PC). Para o ARM usa o addr2line/nm do toolchain do Miyoo.
PROF="$1"
BIN="$2"
OUT="${3:-/mnt/c/Users/Régis/Desktop/oot_perfil.txt}"
if [ ! -f "$PROF" ] || [ ! -f "$BIN" ]; then
    echo "uso: sh $0 AMOSTRAS.txt EXECUTAVEL [SAIDA.txt]"
    exit 1
fi
BASE="$HOME/fallout-miyoo"
A2L=addr2line
NM=nm
if file "$BIN" 2>/dev/null | grep -q "ARM"; then
    T="$(find "$BASE/miyoomini-toolchain" -maxdepth 3 -name arm-linux-gnueabihf-addr2line 2>/dev/null | head -1)"
    if [ -z "$T" ]; then
        echo "ERRO: arm-linux-gnueabihf-addr2line nao encontrado em $BASE/miyoomini-toolchain"
        exit 1
    fi
    A2L="$T"
    NM="$(dirname "$T")/arm-linux-gnueabihf-nm"
fi
LINK=$($NM "$BIN" | awk '$3 == "__executable_start" { print $1 }')
if [ -z "$LINK" ]; then
    echo "ERRO: simbolo __executable_start nao encontrado em $BIN"
    exit 1
fi
TMP="$(mktemp -d)" || exit 1

# 1) cada amostra vira um endereco do executavel (amostras dentro de bibliotecas como libm/libc
#    usam o endereco de retorno: o tempo fica com a linha que chamou a biblioteca)
awk -v link="$LINK" '
function hex(s,   i, c, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) { c = index("0123456789abcdef", substr(s, i, 1)) - 1; v = v * 16 + c } return v }
NR == 1 { base = hex($2); etext = hex($3); lk = hex(link); next }
{
    pc = hex($1); lr = hex($2); tag = "";
    if (pc >= base && pc < etext) a = pc;
    else if (lr >= base && lr < etext) { a = lr; tag = "B" }
    else { print "X" > "/dev/stderr"; next }
    printf "%s%x\n", tag, a - base + lk
}' "$PROF" > "$TMP/addr" 2> "$TMP/fora"
N=$(wc -l < "$TMP/addr")
FORA=$(wc -l < "$TMP/fora")
sed 's/^B//' "$TMP/addr" | sort -u > "$TMP/uniq"
# 2) resolve cada endereco unico (funcao mais interna + arquivo:linha)
"$A2L" -f -e "$BIN" < "$TMP/uniq" | paste - - > "$TMP/sym"
paste "$TMP/uniq" "$TMP/sym" > "$TMP/map"
# 3) soma
awk -v n="$N" -v fora="$FORA" -v out="$OUT" '
FNR == NR { split($0, f, "\t"); fn[f[1]] = f[2]; ln = f[3]; sub(/.*\//, "", ln); sub(/ \(discriminator.*/, "", ln); li[f[1]] = ln; next }
{
    lib = substr($1, 1, 1) == "B"; a = $1; if (lib) a = substr(a, 2);
    k = fn[a]; if (lib) k = k " (dentro de biblioteca)";
    byf[k]++;
    l = li[a] "  [" fn[a] "]"; if (lib) l = l " (biblioteca)";
    byl[l]++
}
END {
    printf "amostras: %d (mais %d fora do executavel e sem endereco de retorno)\n\n", n, fora > out;
    printf "== por funcao (a mais interna, ja contando o que foi expandido inline nela) ==\n" > out;
    close(out);
    for (k in byf) printf "%6.2f%%  %6d  %s\n", 100 * byf[k] / n, byf[k], k | "sort -rn | head -40 >> \"" out "\"";
    close("sort -rn | head -40 >> \"" out "\"");
    printf "\n== por linha ==\n" >> out;
    close(out);
    for (k in byl) printf "%6.2f%%  %6d  %s\n", 100 * byl[k] / n, byl[k], k | "sort -rn | head -70 >> \"" out "\"";
    close("sort -rn | head -70 >> \"" out "\"");
}' "$TMP/map" "$TMP/addr"
# 4) por etapa: cada amostra vai para a funcao MAIS EXTERNA (o que foi expandido inline conta para quem
#    chamou). Dentro do gx_span_fast, a linha decide a etapa (comentarios "--- N. nome ---" do codigo).
SRC="$(cd "$(dirname "$0")/.." && pwd)/port/gfx/gfx_fast.inc"
"$A2L" -a -i -f -e "$BIN" < "$TMP/uniq" > "$TMP/inl"
awk '
/\/\* --- [0-9]\. / { m = $0; sub(/.*\/\* --- /, "", m); sub(/ ---.*/, "", m); nm++; ml[nm] = FNR; mn[nm] = m }
/^static void gx_span_fast\(/ { nm++; ml[nm] = FNR; mn[nm] = "0. preparo do trecho" }
/^    next:/ { nm++; ml[nm] = FNR; mn[nm] = "8. fim do bloco" }
END { for (i = 1; i <= nm; i++) print ml[i] "\t" mn[i] }' "$SRC" | sort -n > "$TMP/marks"
awk -v n="$N" -v out="$OUT" '
FILENAME == ARGV[1] { mk++; mline[mk] = $1; mname[mk] = substr($0, index($0, "\t") + 1); next }
FILENAME == ARGV[2] {
    if ($0 ~ /^0x/) { if (cur != "") lab[cur] = label(); cur = tolower($0); sub(/^0x0*/, "", cur); np = 0; next }
    np++; fr[np] = $0; next
}
FNR == 1 && cur != "" { lab[cur] = label(); cur = "" }
{ a = $1; sub(/^B/, "", a); k = (a in lab) ? lab[a] : "?"; by[k]++ }
function label(   f, l, i, s) {
    f = fr[np - 1]; l = fr[np]; sub(/.*:/, "", l); sub(/ .*/, "", l);
    if (f != "gx_span_fast") return f;
    s = "?"; for (i = 1; i <= mk; i++) if (l + 0 >= mline[i]) s = mname[i];
    return "gx_span_fast: " s
}
END {
    printf "\n== por etapa (inline conta para quem chamou) ==\n" >> out; close(out);
    for (k in by) printf "%6.2f%%  %6d  %s\n", 100 * by[k] / n, by[k], k | "sort -rn | head -40 >> \"" out "\"";
    close("sort -rn | head -40 >> \"" out "\"");
}' "$TMP/marks" "$TMP/inl" "$TMP/addr"
rm -rf "$TMP"
echo "relatorio: $OUT"
head -45 "$OUT"
