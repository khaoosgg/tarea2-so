#ifndef BOARD_H
#define BOARD_H

/*
 * board.h — Tablero de Komikan compartido entre procesos (Paso 4, v02)
 *
 * ESQUEMA DE SINCRONIZACION (resumen para el informe)
 * ---------------------------------------------------
 *  - Un mutex por casilla (cell_lock[i]). Es el "slot" que las fichas compiten
 *    por reclamar: quien toma el lock de la casilla destino primero, se la queda.
 *  - Un mutex de estado (state_lock) + variable de condicion (state_cond) para
 *    lo global de la partida: perros vivos, capturas, resultado y log de eventos.
 *  - Todos los mutex son PROCESS_SHARED (viven en memoria compartida y los usan
 *    el proceso Juez, el proceso Puma y el proceso Perros) y ROBUST (si un
 *    proceso muere con un lock tomado, el siguiente que lo pida recibe
 *    EOWNERDEAD y puede recuperarlo en vez de quedarse bloqueado para siempre).
 *
 *  ORDEN GLOBAL DE BLOQUEO (previene deadlock):
 *    1) locks de casilla en orden ASCENDENTE de indice
 *    2) state_lock SIEMPRE al final
 *    Nunca se pide un lock de casilla teniendo state_lock.
 *
 *  REGLA DE ACCESO a occupant[i]:
 *    - Para ESCRIBIR occupant[i]: tener cell_lock[i] Y state_lock.
 *    - Para LEER occupant[i]: basta tener cell_lock[i] O state_lock.
 *    Asi, una ficha valida su jugada con los locks de casilla, y el Juez puede
 *    tomar una foto consistente de todo el tablero con solo state_lock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

#define NUM_NODES   28
#define NUM_DOGS    12
#define DOGS_TO_WIN 6
#define PUMA_ID     NUM_DOGS   /* ids de ficha: 0..11 perros, 12 puma */
#define NO_PIECE    (-1)
#define EVENT_RING  64         /* eventos recientes guardados para el log del Juez */

typedef enum {
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT,
    DIR_UP_LEFT,
    DIR_UP_RIGHT,
    DIR_DOWN_LEFT,
    DIR_DOWN_RIGHT,
    NUM_DIRS
} Direction;

typedef enum {
    CELL_EMPTY = 0,
    CELL_PUMA,
    CELL_DOG
} CellState;

typedef enum {
    GAME_ONGOING = 0,
    GAME_PUMA_WINS,          /* capturo 6 o mas perros */
    GAME_DOGS_WIN,           /* puma acorralado */
    GAME_ABORTED_PUMA_WINS,  /* el jugador Perros abandono/fallo */
    GAME_ABORTED_DOGS_WIN,   /* el jugador Puma abandono/fallo */
    GAME_ABORTED_SERVER      /* Ctrl+C del servidor o limite de seguridad */
} GameStatus;

typedef enum {
    MOVE_OK = 0,
    MOVE_INVALID,    /* no cumple las reglas de adyacencia/salto */
    MOVE_TAKEN,      /* perdio la carrera: la casilla ya no esta como se vio */
    MOVE_NOT_MINE,   /* la ficha ya no esta en 'from' (ej.: perro capturado) */
    MOVE_GAME_OVER,  /* la partida ya termino */
    MOVE_ERROR       /* un lock tenia dueno muerto (EOWNERDEAD) u otro error */
} MoveResult;

typedef enum {
    EV_MOVE = 0,
    EV_CAPTURE,
    EV_END
} EventType;

typedef struct {
    unsigned long seq;   /* numero correlativo del evento (1, 2, 3...) */
    EventType type;
    int piece;           /* quien actuo (0..11 perro, PUMA_ID, -1 = Juez) */
    int from, over, to;  /* over/victim solo en capturas */
    int victim;
    GameStatus result;   /* solo en EV_END */
} Event;

/* Vive en memoria compartida (mmap MAP_SHARED). No copiar por valor. */
typedef struct {
    pthread_mutex_t cell_lock[NUM_NODES];
    int occupant[NUM_NODES];          /* NO_PIECE, 0..11 o PUMA_ID */

    pthread_mutex_t state_lock;
    pthread_cond_t  state_cond;       /* se avisa al Juez en cada evento */
    bool dog_alive[NUM_DOGS];
    int captured_dogs;
    GameStatus status;
    unsigned long seq;                /* cantidad de eventos registrados */
    Event events[EVENT_RING];         /* buffer circular de eventos */

    atomic_int owner_died;            /* veces que se recupero un lock (EOWNERDEAD) */
} Board;

/* Foto consistente del tablero, copiada bajo state_lock. Es memoria local. */
typedef struct {
    int occupant[NUM_NODES];
    bool dog_alive[NUM_DOGS];
    int captured_dogs;
    GameStatus status;
    unsigned long seq;
    int n_events;                     /* eventos nuevos desde la ultima foto */
    int lost_events;                  /* eventos que se sobrescribieron sin leer */
    Event events[EVENT_RING];
    int owner_died;
} BoardSnapshot;

/* ---------- Topologia y reglas (funciones puras, sin locks) ---------- */
int       board_get_neighbor(int node, Direction dir);
bool      board_are_adjacent(int a, int b);
int       board_jump_landing(int from, int over);   /* destino del salto, o -1 */
CellState board_cell_state(int occupant);
int       board_find_puma(const int occupant[NUM_NODES]);
bool      board_puma_has_moves(const int occupant[NUM_NODES]);

/* ---------- Ciclo de vida en memoria compartida ---------- */
/* Crea el tablero con mmap(MAP_SHARED | MAP_ANONYMOUS). Debe llamarse ANTES de
 * hacer fork de los jugadores, para que lo hereden. NULL si hay error o si las
 * posiciones iniciales son invalidas (fuera de rango o repetidas). */
Board *board_create_shared(int puma_start, const int dog_start[NUM_DOGS]);

/* Libera y destruye todas las primitivas y hace munmap.
 * Precondicion: todos los procesos jugadores ya fueron cosechados (waitpid).
 * Retorna cuantos locks seguian tomados por un dueno muerto (se recuperaron
 * antes de destruirlos), o -1 si algun lock seguia tomado por alguien vivo. */
int board_destroy_shared(Board *b);

/* ---------- Operaciones concurrentes (las usan las fichas) ---------- */
MoveResult board_try_move(Board *b, int piece_id, int from, int to);
MoveResult board_try_jump(Board *b, int from, int over, int to);  /* solo el puma */

/* Lectura optimista: copia la ocupacion para DECIDIR una jugada.
 * La jugada se revalida despues dentro de board_try_move/board_try_jump. */
void board_peek(Board *b, int occupant_out[NUM_NODES], GameStatus *status_out);

GameStatus board_get_status(Board *b);

/* El Juez fija un resultado (abandono, Ctrl+C) si la partida sigue en curso. */
bool board_finish(Board *b, GameStatus result);

/* Para el Juez: espera hasta que haya eventos nuevos o pasen timeout_ms, y
 * luego copia tablero + eventos nuevos en una sola seccion critica.
 * Retorna true si hubo eventos nuevos. Actualiza *last_seq. */
bool board_wait_and_snapshot(Board *b, unsigned long *last_seq, int timeout_ms,
                             BoardSnapshot *out);

/* ---------- Presentacion ---------- */
const char *board_status_str(GameStatus s);
/* Escribe eventos + dibujo del tablero en buf (para imprimirlo con UN solo
 * write y que no se mezcle con la salida de otras mesas). */
void board_format(const BoardSnapshot *s, int mesa_id, char *buf, size_t len);

#endif
