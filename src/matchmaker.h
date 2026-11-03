#ifndef MATCHMAKER_H
#define MATCHMAKER_H

/*
 * matchmaker.h — Servidor de emparejamiento (Paso 7) + Ctrl+C (Paso 9)
 *
 * Hilos del proceso servidor:
 *   - N hilos CLIENTE: cada uno espera un retardo aleatorio (llegada), se
 *     anota en la cola de su rol (pares = Puma, impares = Perros) y espera
 *     dormido (pthread_cond_wait) hasta que lo asignen a una mesa.
 *   - 1 hilo MATCHMAKER: saca un Puma y un Perros de las colas, hace
 *     sem_wait sobre el semaforo 'mesas_libres' (valor inicial M), y hace
 *     fork() de un proceso Mesa. Asi nunca hay mas de M mesas activas, y los
 *     pares sobrantes esperan bloqueados en el semaforo (espera pasiva).
 *   - 1 hilo COSECHADOR: hace waitpid de las mesas que terminan, anota el
 *     resultado y hace sem_post (libera la mesa para el siguiente par).
 *   - 1 hilo de SENALES: espera SIGINT/SIGTERM con sigwait. Al llegar Ctrl+C:
 *     deja de aceptar clientes, avisa a cada mesa activa con SIGTERM para que
 *     termine ordenadamente, y despierta a todos los hilos para que salgan.
 *
 * Retorna 0 si el torneo termino normal, 130 si se interrumpio con Ctrl+C.
 */

#include "config.h"

int matchmaker_run(int N, int M, const Config *cfg);

#endif
