#include <stdio.h>
#include <stdlib.h>
#include "board.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <N_jugadores> <M_mesas> [config.txt]\n", argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    printf("[SERVIDOR] Iniciando Torneo Komikan: N=%d clientes, M=%d mesas max.\n", N, M);

    Board test_board;
    board_init(&test_board);
    board_print(&test_board, 1, "Tablero Inicial de Prueba");
    board_destroy(&test_board);

    return 0;
}