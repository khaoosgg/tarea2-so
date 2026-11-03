#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "game.h"

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <N_jugadores> <M_mesas> [config.txt]\n", argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);

    printf("[SERVIDOR] Iniciando Torneo Komikan: N=%d clientes, M=%d mesas max.\n", N, M);
    
    // Inicializar generador de números aleatorios global
    srand((unsigned int)time(NULL));

    // Probamos ejecutando la simulación en la Mesa 1
    run_table_simulation(1);

    return 0;
}