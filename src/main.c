/*
 * main.c — Servidor del Torneo de Komikan Concurrente
 *
 *   ./torneo N M [config.txt]
 *     N = numero total de clientes (jugadores) a procesar  (N >= 2)
 *     M = maximo de mesas activas al mismo tiempo           (M >= 1)
 */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "matchmaker.h"

static int leer_entero(const char *txt, long minimo, long maximo, int *out) {
    char *end;
    errno = 0;
    long v = strtol(txt, &end, 10);
    if (errno != 0 || end == txt || *end != '\0' || v < minimo || v > maximo) return -1;
    *out = (int)v;
    return 0;
}

int main(int argc, char *argv[]) {
    int N, M;
    if (argc < 3 || argc > 4 ||
        leer_entero(argv[1], 2, 100000, &N) != 0 ||
        leer_entero(argv[2], 1, 10000, &M) != 0) {
        fprintf(stderr, "Uso: %s <N_jugadores (>=2)> <M_mesas (>=1)> [config.txt]\n", argv[0]);
        return 1;
    }

    Config cfg;
    config_defaults(&cfg);
    if (argc == 4 && config_load(argv[3], &cfg) != 0) {
        fprintf(stderr, "Configuracion invalida, se cancela el torneo.\n");
        return 1;
    }
    if (log_open(cfg.log_file) != 0) {
        perror("No se pudo abrir el archivo de log");
        return 1;
    }

    log_msg("[SERVIDOR] Torneo Komikan Concurrente: N=%d clientes, M=%d mesas max. (pid %d)\n",
            N, M, (int)getpid());
    config_print(&cfg, stdout);
    fflush(stdout);

    int r = matchmaker_run(N, M, &cfg);

    log_close();
    return r;
}
