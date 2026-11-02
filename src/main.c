#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <N_jugadores> <M_mesas> [config.txt]\n", argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    int M = atoi(argv[2]);
    const char* config_file = (argc >= 4) ? argv[3] : NULL;

    if (N <= 0 || M <= 0) {
        fprintf(stderr, "Error: N y M deben ser mayores a 0.\n");
        return 1;
    }

    printf("[SERVIDOR] Iniciando Torneo Komikan: N=%d clientes, M=%d mesas max.\n", N, M);
    if (config_file) {
        printf("[SERVIDOR] Archivo de configuracion: %s\n", config_file);
    }

    return 0;
}
