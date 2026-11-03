/*
 * jugador.c — Fichas del Komikan como hilos (Paso 5)
 * Ver el resumen en jugador.h.
 */
#include "jugador.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ===================================================================== */
/*  Distancias en el grafo (BFS desde cada nodo, una sola vez)            */
/* ===================================================================== */

static int dist[NUM_NODES][NUM_NODES];
static pthread_once_t dist_once = PTHREAD_ONCE_INIT;

static void calcular_distancias(void) {
    for (int s = 0; s < NUM_NODES; s++) {
        for (int i = 0; i < NUM_NODES; i++) dist[s][i] = -1;
        int cola[NUM_NODES], ini = 0, fin = 0;
        dist[s][s] = 0;
        cola[fin++] = s;
        while (ini < fin) {
            int u = cola[ini++];
            for (int d = 0; d < NUM_DIRS; d++) {
                int v = board_get_neighbor(u, (Direction)d);
                if (v != -1 && dist[s][v] == -1) {
                    dist[s][v] = dist[s][u] + 1;
                    cola[fin++] = v;
                }
            }
        }
    }
}

int jugador_distancia(int a, int b) {
    pthread_once(&dist_once, calcular_distancias);
    if (a < 0 || a >= NUM_NODES || b < 0 || b >= NUM_NODES) return 99;
    return dist[a][b];
}

/* ===================================================================== */
/*  Utilidades                                                            */
/* ===================================================================== */

static bool es_perro(int occ) { return occ >= 0 && occ < NUM_DOGS; }

/* Duerme un tiempo aleatorio entre min y max ms (nanosleep: no usa senales) */
static void dormir_aleatorio(const JugadorParams *p, unsigned *seed) {
    int lo = p->delay_min_ms, hi = p->delay_max_ms;
    if (hi < lo) hi = lo;
    int ms = lo + (hi > lo ? (int)(rand_r(seed) % (unsigned)(hi - lo + 1)) : 0);
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0) { /* reintenta si una senal lo corta */ }
}

/* Casillas vacias alrededor de 'nodo' en la ocupacion 'occ' */
static int libres_alrededor(const int occ[NUM_NODES], int nodo) {
    int n = 0;
    for (int d = 0; d < NUM_DIRS; d++) {
        int v = board_get_neighbor(nodo, (Direction)d);
        if (v != -1 && occ[v] == NO_PIECE) n++;
    }
    return n;
}

/* ¿Puede el puma (en 'puma') saltar sobre la casilla 'nodo' con la ocupacion
 * 'occ'? Es decir: puma adyacente a 'nodo' y la casilla siguiente en esa misma
 * linea recta esta vacia. */
static bool amenazada(const int occ[NUM_NODES], int puma, int nodo) {
    if (puma < 0) return false;
    for (int d = 0; d < NUM_DIRS; d++) {
        if (board_get_neighbor(puma, (Direction)d) == nodo) {
            int land = board_get_neighbor(nodo, (Direction)d);
            return land != -1 && occ[land] == NO_PIECE;
        }
    }
    return false;
}

/* Falla a proposito (Paso 8): toma el lock de una casilla y el proceso entero
 * muere con SIGKILL sin soltarlo. El Juez debe detectarlo con waitpid y el
 * tablero recupera el lock gracias a que los mutex son ROBUST. */
static void fallar_con_lock_tomado(Board *b, int casilla) {
    pthread_mutex_lock(&b->cell_lock[casilla]);
    kill(getpid(), SIGKILL);
    for (;;) pause();                      /* no se llega aqui */
}

/* Cuenta jugadas exitosas del proceso jugador (lo comparten sus hilos) */
static atomic_int jugadas_del_proceso;

static void quizas_fallar(Board *b, const JugadorParams *p, int pos_actual) {
    if (p->fail_after_moves <= 0) return;
    if (atomic_fetch_add(&jugadas_del_proceso, 1) + 1 >= p->fail_after_moves)
        fallar_con_lock_tomado(b, pos_actual);
}

/* Lee el tablero hasta que la ficha aparezca o termine la partida */
static int buscar_ficha(Board *b, int id) {
    int occ[NUM_NODES]; GameStatus st;
    board_peek(b, occ, &st);
    for (int i = 0; i < NUM_NODES; i++)
        if (occ[i] == id) return i;
    return -1;
}

/* ===================================================================== */
/*  Puma                                                                  */
/* ===================================================================== */

typedef struct { Board *b; const JugadorParams *p; } PumaArgs;

static void *hilo_puma(void *arg) {
    PumaArgs *a = arg;
    Board *b = a->b;
    const JugadorParams *p = a->p;
    unsigned seed = p->seed * 7919u + (unsigned)p->mesa_id * 131u + PUMA_ID;

    int pos = buscar_ficha(b, PUMA_ID);
    if (pos < 0) return NULL;

    for (;;) {
        dormir_aleatorio(p, &seed);

        int occ[NUM_NODES]; GameStatus st;
        board_peek(b, occ, &st);
        if (st != GAME_ONGOING) break;

        /* 1) Capturas posibles: elegir la de aterrizaje con mas espacio */
        int best_over = -1, best_to = -1, best_score = -1, empates = 0;
        for (int d = 0; d < NUM_DIRS; d++) {
            int v = board_get_neighbor(pos, (Direction)d);
            if (v == -1 || !es_perro(occ[v])) continue;
            int land = board_get_neighbor(v, (Direction)d);
            if (land == -1 || occ[land] != NO_PIECE) continue;
            int score = libres_alrededor(occ, land);
            if (score > best_score) { best_score = score; best_over = v; best_to = land; empates = 1; }
            else if (score == best_score && rand_r(&seed) % (unsigned)(++empates) == 0) {
                best_over = v; best_to = land;
            }
        }

        MoveResult r;
        int destino;
        if (best_to != -1) {
            destino = best_to;
            r = board_try_jump(b, pos, best_over, best_to);
        } else {
            /* 2) Sin captura: ir a la vecina con mas salidas (+ si amenaza perros) */
            int best = -1; best_score = -1; empates = 0;
            for (int d = 0; d < NUM_DIRS; d++) {
                int v = board_get_neighbor(pos, (Direction)d);
                if (v == -1 || occ[v] != NO_PIECE) continue;
                int copia[NUM_NODES];
                memcpy(copia, occ, sizeof copia);
                copia[pos] = NO_PIECE;
                copia[v] = PUMA_ID;
                int amenazas = 0;
                for (int k = 0; k < NUM_NODES; k++)
                    if (es_perro(copia[k]) && amenazada(copia, v, k)) amenazas++;
                int score = libres_alrededor(copia, v) + 2 * amenazas;
                if (score > best_score) { best_score = score; best = v; empates = 1; }
                else if (score == best_score && rand_r(&seed) % (unsigned)(++empates) == 0)
                    best = v;
            }
            if (best == -1) continue;      /* sin jugadas: el tablero ya lo detecto */
            destino = best;
            r = board_try_move(b, PUMA_ID, pos, best);
        }

        if (r == MOVE_OK) {
            pos = destino;
            quizas_fallar(b, p, pos);
        } else if (r == MOVE_NOT_MINE || r == MOVE_GAME_OVER) {
            break;                         /* fin de partida */
        }
        /* MOVE_TAKEN: perdio la carrera. MOVE_ERROR: el otro jugador murio con
         * un lock tomado; NO salimos (no es culpa nuestra): seguimos hasta que
         * el Juez declare el abandono y el proximo peek vea la partida cerrada. */
    }
    return NULL;
}

int jugador_puma_run(Board *b, const JugadorParams *p) {
    signal(SIGINT, SIG_IGN);
    atomic_store(&jugadas_del_proceso, 0);
    PumaArgs a = { b, p };
    pthread_t th;
    if (pthread_create(&th, NULL, hilo_puma, &a) != 0) return 1;
    pthread_join(th, NULL);
    return 0;
}

/* ===================================================================== */
/*  Perros                                                                */
/* ===================================================================== */

typedef struct { Board *b; const JugadorParams *p; int id; } PerroArgs;

static void *hilo_perro(void *arg) {
    PerroArgs *a = arg;
    Board *b = a->b;
    const JugadorParams *p = a->p;
    int id = a->id;
    unsigned seed = p->seed * 7919u + (unsigned)p->mesa_id * 131u + (unsigned)id;

    int pos = buscar_ficha(b, id);
    if (pos < 0) return NULL;

    for (;;) {
        dormir_aleatorio(p, &seed);

        int occ[NUM_NODES]; GameStatus st;
        board_peek(b, occ, &st);
        if (st != GAME_ONGOING || occ[pos] != id) break;   /* fin o capturado */

        int puma = board_find_puma(occ);
        bool en_peligro = amenazada(occ, puma, pos);
        int d_actual = jugador_distancia(pos, puma);

        /* Evaluar cada vecina vacia */
        int best = -1, best_score = -1000, best_dist = 99, empates = 0;
        for (int d = 0; d < NUM_DIRS; d++) {
            int v = board_get_neighbor(pos, (Direction)d);
            if (v == -1 || occ[v] != NO_PIECE) continue;
            int copia[NUM_NODES];
            memcpy(copia, occ, sizeof copia);
            copia[pos] = NO_PIECE;
            copia[v] = id;
            if (amenazada(copia, puma, v)) continue;      /* nunca a una casilla expuesta */
            int score = -jugador_distancia(v, puma) * 10;
            /* no dejar expuesto a un compañero al moverse */
            for (int k = 0; k < NUM_NODES; k++)
                if (es_perro(copia[k]) && k != v && amenazada(copia, puma, k)) score -= 15;
            if (score > best_score) {
                best_score = score; best = v; best_dist = jugador_distancia(v, puma); empates = 1;
            } else if (score == best_score && rand_r(&seed) % (unsigned)(++empates) == 0) {
                best = v; best_dist = jugador_distancia(v, puma);
            }
        }

        /* ¿Conviene quedarse? Si estoy seguro y moverme no me acerca, me quedo.
         * Un 10% de las veces se mueve igual, para no congelar la partida. */
        bool quedarse = false;
        if (best == -1) quedarse = true;
        else if (!en_peligro && best_dist >= d_actual && rand_r(&seed) % 10 != 0)
            quedarse = true;
        if (quedarse) continue;

        MoveResult r = board_try_move(b, id, pos, best);
        if (r == MOVE_OK) {
            pos = best;
            quizas_fallar(b, p, pos);
        } else if (r == MOVE_NOT_MINE || r == MOVE_GAME_OVER) {
            break;                         /* capturado o fin de partida */
        }
        /* MOVE_TAKEN o MOVE_ERROR: reintentar en el proximo ciclo (ver puma) */
    }
    return NULL;
}

int jugador_perros_run(Board *b, const JugadorParams *p) {
    signal(SIGINT, SIG_IGN);
    atomic_store(&jugadas_del_proceso, 0);
    PerroArgs args[NUM_DOGS];
    pthread_t th[NUM_DOGS];
    int creados = 0;
    for (int i = 0; i < NUM_DOGS; i++) {
        args[i] = (PerroArgs){ b, p, i };
        if (pthread_create(&th[i], NULL, hilo_perro, &args[i]) != 0) break;
        creados++;
    }
    for (int i = 0; i < creados; i++) pthread_join(th[i], NULL);
    return creados == NUM_DOGS ? 0 : 1;
}
