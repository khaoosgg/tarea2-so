#ifndef JUGADOR_H
#define JUGADOR_H

/*
 * jugador.h — Fichas del Komikan como hilos (Paso 5)
 *
 * Cada jugador es un PROCESO hijo de la mesa (lo crea el Juez con fork):
 *   - Jugador Puma:   1 hilo  (la ficha del puma)
 *   - Jugador Perros: 12 hilos (un hilo por perro)
 *
 * Ciclo de cada ficha:
 *   dormir [delay_min, delay_max] ms -> board_peek (mirar) -> decidir ->
 *   board_try_move / board_try_jump (intentar; el tablero revalida) ->
 *   MOVE_TAKEN: perdio la carrera, vuelve a intentar en el proximo ciclo.
 *   MOVE_ERROR (lock con dueno muerto): reintenta; el Juez cerrara la partida.
 *   MOVE_NOT_MINE / MOVE_GAME_OVER: el hilo termina.
 *
 * Estrategia (Decision C, opcion 2):
 *   - Puma: si puede capturar, captura (eligiendo el aterrizaje con mas
 *     espacio libre). Si no, va a la casilla vecina con mas salidas.
 *   - Perros: si estan amenazados, escapan a una casilla segura. Si no, se
 *     acercan al puma solo por casillas donde no queden expuestos a un salto.
 *     A distancia 1 del puma y seguros, se quedan quietos (cierran el cerco).
 */

#include "board.h"

typedef struct {
    int mesa_id;
    int delay_min_ms;        /* retardo minimo entre jugadas (ms) */
    int delay_max_ms;        /* retardo maximo entre jugadas (ms) */
    unsigned seed;           /* semilla base; cada ficha deriva la suya */
    int fail_after_moves;    /* <= 0: nunca falla. N > 0: tras N jugadas propias
                                el proceso jugador muere con SIGKILL MIENTRAS
                                tiene tomado un lock del tablero (Paso 8) */
} JugadorParams;

/* Se llaman DENTRO del proceso hijo, despues del fork. Bloquean hasta que la
 * partida termina (o la ficha es capturada / el proceso falla a proposito).
 * Ignoran SIGINT: el unico que reacciona a Ctrl+C es el servidor (Paso 9).
 * Retornan 0 si todo fue normal. */
int jugador_puma_run(Board *b, const JugadorParams *p);
int jugador_perros_run(Board *b, const JugadorParams *p);

/* Distancia minima (en pasos) entre dos nodos del tablero, sin contar fichas. */
int jugador_distancia(int a, int b);

#endif
