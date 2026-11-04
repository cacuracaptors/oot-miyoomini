/* oot-mkcart ROM.z64 saida.cart oot_swapmap.bin : cria o cartucho virtual (so para testes no PC). */
#include <stdio.h>
#include "cart_build.h"
static void cb(void* ud, const char* s, int p) {
    (void)ud;
    fprintf(stderr, "\r  %-34s %3d%%", s, p);
    if (p >= 100) fputc('\n', stderr);
}
int main(int argc, char** argv) {
    char err[512] = "";
    if (argc != 4) {
        fprintf(stderr, "uso: %s ROM.z64 saida.cart oot_swapmap.bin\n", argv[0]);
        return 2;
    }
    if (cart_build(argv[1], argv[2], argv[3], cb, NULL, err, sizeof(err)) != 0) {
        fprintf(stderr, "\nERRO: %s\n", err);
        return 1;
    }
    return 0;
}
