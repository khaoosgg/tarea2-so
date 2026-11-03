#ifndef MESA_H
#define MESA_H

/*
 * mesa.h — CONTRATO de la Mesa / Juez (Paso 6)
 *
 * Quien lo usa: el matchmaker (Paso 7). Por cada par Puma+Perros:
 *
 *     sem_wait(&mesas_libres);              // espera pasiva si hay M mesas ocupadas
 *     pid_t pid = fork();
 *     if (pid == 0) _exit(mesa_run(id, cliente_puma, cliente_perros, &cfg));
 *     // ... el padre guarda pid; cuando waitpid(pid) lo cosecha -> sem_post
 *
 * mesa_run se ejecuta DENTRO del proceso hijo (la mesa). Crea el tablero en
 * memoria compartida, hace fork de los jugadores Puma y Perros, arbitra,
 * imprime el log y libera todo antes de retornar.
 *
 * Valor de retorno (= codigo de salida del proceso mesa): un GameStatus
 *   1 GAME_PUMA_WINS, 2 GAME_DOGS_WIN, 3 GAME_ABORTED_PUMA_WINS,
 *   4 GAME_ABORTED_DOGS_WIN, 5 GAME_ABORTED_SERVER;  >= 10 = error interno.
 * El matchmaker lo lee con WEXITSTATUS(status) para el resumen final.
 *
 * Ctrl+C (Paso 9): el servidor envia SIGTERM a cada proceso mesa activo.
 * La mesa lo trata como "concluir de forma ordenada": declara la partida
 * GAME_ABORTED_SERVER, espera a sus jugadores, libera y retorna 5.
 */

#include "config.h"

int mesa_run(int mesa_id, int cliente_puma, int cliente_perros, const Config *cfg);

#endif
