/*
 * board.c — Tablero de Komikan compartido entre procesos (Paso 4, v02)
 * Ver el resumen del esquema de sincronizacion en board.h.
 */
#include "board.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

/* ===================================================================== */
/*  Topologia (28 nodos: 0 = trono, 1-2 = triangulo, 3-27 = grilla 5x5)   */
/* ===================================================================== */

static int neighbors[NUM_NODES][NUM_DIRS];
static pthread_once_t graph_once = PTHREAD_ONCE_INIT;

static void link_nodes(int a, Direction d_ab, int b, Direction d_ba) {
    neighbors[a][d_ab] = b;
    neighbors[b][d_ba] = a;
}

static void init_graph_topology(void) {
    for (int i = 0; i < NUM_NODES; i++)
        for (int d = 0; d < NUM_DIRS; d++)
            neighbors[i][d] = -1;

    /* Triangulo / trono */
    link_nodes(0, DIR_DOWN_LEFT,  1, DIR_UP_RIGHT);
    link_nodes(0, DIR_DOWN_RIGHT, 2, DIR_UP_LEFT);
    link_nodes(1, DIR_DOWN_LEFT,  3, DIR_UP_RIGHT);
    link_nodes(2, DIR_DOWN_RIGHT, 7, DIR_UP_LEFT);
    link_nodes(1, DIR_RIGHT,      2, DIR_LEFT);
    link_nodes(1, DIR_DOWN,       4, DIR_UP);
    link_nodes(0, DIR_DOWN,       5, DIR_UP);   /* cruza la linea 1-2 SIN nodo */
    link_nodes(2, DIR_DOWN,       6, DIR_UP);

    /* Grilla 5x5 (nodos 3..27), solo ortogonal */
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            int curr = 3 + row * 5 + col;
            if (col < 4) link_nodes(curr, DIR_RIGHT, curr + 1, DIR_LEFT);
            if (row < 4) link_nodes(curr, DIR_DOWN,  curr + 5, DIR_UP);
        }
    }
}

/* pthread_once: se ejecuta una sola vez aunque varios hilos llamen a la vez
 * (corrige la condicion de carrera de la bandera graph_initialized). */
static void ensure_graph(void) {
    pthread_once(&graph_once, init_graph_topology);
}

static bool valid_node(int n) { return n >= 0 && n < NUM_NODES; }
static bool is_dog(int occ)   { return occ >= 0 && occ < NUM_DOGS; }

int board_get_neighbor(int node, Direction dir) {
    ensure_graph();
    if (!valid_node(node) || dir < 0 || dir >= NUM_DIRS) return -1;
    return neighbors[node][dir];
}

bool board_are_adjacent(int a, int b) {
    ensure_graph();
    if (!valid_node(a) || !valid_node(b)) return false;
    for (int d = 0; d < NUM_DIRS; d++)
        if (neighbors[a][d] == b) return true;
    return false;
}

int board_jump_landing(int from, int over) {
    ensure_graph();
    if (!valid_node(from) || !valid_node(over)) return -1;
    for (int d = 0; d < NUM_DIRS; d++)
        if (neighbors[from][d] == over) return neighbors[over][d];
    return -1;
}

CellState board_cell_state(int occ) {
    if (occ == PUMA_ID) return CELL_PUMA;
    if (is_dog(occ))    return CELL_DOG;
    return CELL_EMPTY;
}

int board_find_puma(const int occ[NUM_NODES]) {
    for (int i = 0; i < NUM_NODES; i++)
        if (occ[i] == PUMA_ID) return i;
    return -1;
}

bool board_puma_has_moves(const int occ[NUM_NODES]) {
    ensure_graph();
    int p = board_find_puma(occ);
    if (p < 0) return false;
    for (int d = 0; d < NUM_DIRS; d++) {
        int adj = neighbors[p][d];
        if (adj == -1) continue;
        if (occ[adj] == NO_PIECE) return true;                 /* paso simple */
        if (is_dog(occ[adj])) {
            int land = neighbors[adj][d];                      /* salto recto */
            if (land != -1 && occ[land] == NO_PIECE) return true;
        }
    }
    return false;
}

/* ===================================================================== */
/*  Locks robustos                                                        */
/* ===================================================================== */

/* Retorna 0 si se tomo normal, 1 si el dueno anterior murio (se recupero con
 * pthread_mutex_consistent y AHORA lo tenemos tomado), -1 si hubo error. */
static int robust_lock(Board *b, pthread_mutex_t *m) {
    int r = pthread_mutex_lock(m);
    if (r == 0) return 0;
    if (r == EOWNERDEAD) {
        pthread_mutex_consistent(m);
        atomic_fetch_add(&b->owner_died, 1);
        return 1;
    }
    return -1;
}

/* Toma los locks de las casillas cells[0..n-1] en orden ascendente.
 * Si alguno tenia dueno muerto, suelta todo y retorna MOVE_ERROR: el estado
 * podria haber quedado a medias y el Juez abortara la partida. */
static MoveResult lock_cells_sorted(Board *b, int *cells, int n) {
    for (int i = 1; i < n; i++) {                 /* insertion sort, n <= 3 */
        int x = cells[i], j = i - 1;
        while (j >= 0 && cells[j] > x) { cells[j + 1] = cells[j]; j--; }
        cells[j + 1] = x;
    }
    for (int i = 0; i < n; i++) {
        int r = robust_lock(b, &b->cell_lock[cells[i]]);
        if (r != 0) {
            if (r == 1) pthread_mutex_unlock(&b->cell_lock[cells[i]]);
            for (int k = i - 1; k >= 0; k--)
                pthread_mutex_unlock(&b->cell_lock[cells[k]]);
            return MOVE_ERROR;
        }
    }
    return MOVE_OK;
}

static void unlock_cells(Board *b, const int *cells, int n) {
    for (int i = n - 1; i >= 0; i--)
        pthread_mutex_unlock(&b->cell_lock[cells[i]]);
}

/* state_lock: si el dueno murio, igual seguimos (ya lo tenemos y es
 * consistente); el contador owner_died deja la evidencia para el Juez. */
static bool lock_state(Board *b) {
    return robust_lock(b, &b->state_lock) >= 0;
}

static void unlock_state(Board *b) {
    pthread_mutex_unlock(&b->state_lock);
}

/* ===================================================================== */
/*  Eventos (requieren state_lock tomado)                                 */
/* ===================================================================== */

static void record_event(Board *b, Event ev) {
    b->seq++;
    ev.seq = b->seq;
    b->events[(b->seq - 1) % EVENT_RING] = ev;
}

static void set_result(Board *b, GameStatus result) {
    b->status = result;
    Event ev = { .type = EV_END, .piece = -1, .from = -1, .over = -1,
                 .to = -1, .victim = -1, .result = result };
    record_event(b, ev);
}

/* Regla de victoria aplicada dentro de la misma seccion critica de la jugada:
 * no existe un instante en que el puma este acorralado y nadie lo note. */
static void check_victory_locked(Board *b) {
    if (b->status != GAME_ONGOING) return;
    if (b->captured_dogs >= DOGS_TO_WIN)       set_result(b, GAME_PUMA_WINS);
    else if (!board_puma_has_moves(b->occupant)) set_result(b, GAME_DOGS_WIN);
}

/* ===================================================================== */
/*  Ciclo de vida                                                         */
/* ===================================================================== */

Board *board_create_shared(int puma_start, const int dog_start[NUM_DOGS]) {
    ensure_graph();

    /* Validar posiciones iniciales: en rango y sin repetir */
    bool used[NUM_NODES] = { false };
    if (!valid_node(puma_start)) return NULL;
    used[puma_start] = true;
    for (int i = 0; i < NUM_DOGS; i++) {
        if (!valid_node(dog_start[i]) || used[dog_start[i]]) return NULL;
        used[dog_start[i]] = true;
    }

    Board *b = mmap(NULL, sizeof(Board), PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) return NULL;
    memset(b, 0, sizeof(Board));

    pthread_mutexattr_t ma;
    pthread_condattr_t ca;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);

    int cells_ok = 0;              /* cuantos cell_lock se inicializaron */
    bool state_ok = false, cond_ok = false;
    while (cells_ok < NUM_NODES &&
           pthread_mutex_init(&b->cell_lock[cells_ok], &ma) == 0)
        cells_ok++;
    if (cells_ok == NUM_NODES)
        state_ok = (pthread_mutex_init(&b->state_lock, &ma) == 0);
    if (state_ok)
        cond_ok = (pthread_cond_init(&b->state_cond, &ca) == 0);

    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);
    if (!cond_ok) {                /* deshacer lo que alcanzo a inicializarse */
        for (int i = 0; i < cells_ok; i++) pthread_mutex_destroy(&b->cell_lock[i]);
        if (state_ok) pthread_mutex_destroy(&b->state_lock);
        munmap(b, sizeof(Board));
        return NULL;
    }

    for (int i = 0; i < NUM_NODES; i++) b->occupant[i] = NO_PIECE;
    b->occupant[puma_start] = PUMA_ID;
    for (int i = 0; i < NUM_DOGS; i++) {
        b->occupant[dog_start[i]] = i;
        b->dog_alive[i] = true;
    }
    b->captured_dogs = 0;
    b->status = GAME_ONGOING;
    b->seq = 0;
    atomic_init(&b->owner_died, 0);

    check_victory_locked(b);   /* por si la disposicion inicial ya acorrala */
    return b;
}

/* Desbloquea (si quedo tomado por un muerto) y destruye un mutex.
 * Retorna 0 si estaba libre, 1 si estaba tomado por un dueno muerto,
 * -1 si lo tiene alguien vivo (no se destruye). */
static int release_and_destroy(pthread_mutex_t *m) {
    int r = pthread_mutex_trylock(m);
    int held_by_dead = 0;
    if (r == EOWNERDEAD) {
        pthread_mutex_consistent(m);
        held_by_dead = 1;
    } else if (r != 0) {
        return -1;                 /* EBUSY: dueno vivo */
    }
    pthread_mutex_unlock(m);
    pthread_mutex_destroy(m);
    return held_by_dead;
}

int board_destroy_shared(Board *b) {
    if (!b) return 0;
    int recovered = 0, busy = 0;
    for (int i = 0; i < NUM_NODES; i++) {
        int r = release_and_destroy(&b->cell_lock[i]);
        if (r < 0) busy++; else recovered += r;
    }
    int r = release_and_destroy(&b->state_lock);
    if (r < 0) busy++; else recovered += r;
    if (r >= 0) pthread_cond_destroy(&b->state_cond);

    munmap(b, sizeof(Board));
    return busy ? -1 : recovered;
}

/* ===================================================================== */
/*  Jugadas                                                               */
/* ===================================================================== */

MoveResult board_try_move(Board *b, int piece_id, int from, int to) {
    if (!valid_node(from) || !valid_node(to) || from == to) return MOVE_INVALID;
    if (piece_id < 0 || piece_id > PUMA_ID) return MOVE_INVALID;
    if (!board_are_adjacent(from, to)) return MOVE_INVALID;

    int cells[2] = { from, to };
    if (lock_cells_sorted(b, cells, 2) != MOVE_OK) return MOVE_ERROR;

    MoveResult res;
    /* Revalidacion pesimista: lo que la ficha vio en board_peek pudo cambiar */
    if (b->occupant[from] != piece_id)      res = MOVE_NOT_MINE;
    else if (b->occupant[to] != NO_PIECE)   res = MOVE_TAKEN;
    else if (!lock_state(b))                res = MOVE_ERROR;
    else {
        if (b->status != GAME_ONGOING) {
            res = MOVE_GAME_OVER;
        } else {
            b->occupant[from] = NO_PIECE;
            b->occupant[to] = piece_id;
            Event ev = { .type = EV_MOVE, .piece = piece_id, .from = from,
                         .over = -1, .to = to, .victim = -1,
                         .result = GAME_ONGOING };
            record_event(b, ev);
            check_victory_locked(b);
            pthread_cond_broadcast(&b->state_cond);
            res = MOVE_OK;
        }
        unlock_state(b);
    }
    unlock_cells(b, cells, 2);
    return res;
}

MoveResult board_try_jump(Board *b, int from, int over, int to) {
    if (!valid_node(from) || !valid_node(over) || !valid_node(to)) return MOVE_INVALID;
    if (board_jump_landing(from, over) != to) return MOVE_INVALID;

    int cells[3] = { from, over, to };
    if (lock_cells_sorted(b, cells, 3) != MOVE_OK) return MOVE_ERROR;

    MoveResult res;
    if (b->occupant[from] != PUMA_ID)       res = MOVE_NOT_MINE;
    else if (!is_dog(b->occupant[over]))    res = MOVE_TAKEN;   /* el perro se fue */
    else if (b->occupant[to] != NO_PIECE)   res = MOVE_TAKEN;
    else if (!lock_state(b))                res = MOVE_ERROR;
    else {
        if (b->status != GAME_ONGOING) {
            res = MOVE_GAME_OVER;
        } else {
            int victim = b->occupant[over];
            b->occupant[from] = NO_PIECE;
            b->occupant[over] = NO_PIECE;
            b->occupant[to] = PUMA_ID;
            b->dog_alive[victim] = false;
            b->captured_dogs++;
            Event ev = { .type = EV_CAPTURE, .piece = PUMA_ID, .from = from,
                         .over = over, .to = to, .victim = victim,
                         .result = GAME_ONGOING };
            record_event(b, ev);
            check_victory_locked(b);
            pthread_cond_broadcast(&b->state_cond);
            res = MOVE_OK;
        }
        unlock_state(b);
    }
    unlock_cells(b, cells, 3);
    return res;
}

/* ===================================================================== */
/*  Lecturas y control del Juez                                           */
/* ===================================================================== */

void board_peek(Board *b, int occ_out[NUM_NODES], GameStatus *status_out) {
    if (!lock_state(b)) {
        if (status_out) *status_out = GAME_ABORTED_SERVER;
        return;
    }
    memcpy(occ_out, b->occupant, sizeof(b->occupant));
    if (status_out) *status_out = b->status;
    unlock_state(b);
}

GameStatus board_get_status(Board *b) {
    if (!lock_state(b)) return GAME_ABORTED_SERVER;
    GameStatus s = b->status;
    unlock_state(b);
    return s;
}

bool board_finish(Board *b, GameStatus result) {
    if (!lock_state(b)) return false;
    bool changed = false;
    if (b->status == GAME_ONGOING && result != GAME_ONGOING) {
        set_result(b, result);
        pthread_cond_broadcast(&b->state_cond);
        changed = true;
    }
    unlock_state(b);
    return changed;
}

bool board_wait_and_snapshot(Board *b, unsigned long *last_seq, int timeout_ms,
                             BoardSnapshot *out) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec  += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    memset(out, 0, sizeof(*out));
    if (!lock_state(b)) { out->status = GAME_ABORTED_SERVER; return false; }

    while (b->seq == *last_seq) {
        int r = pthread_cond_timedwait(&b->state_cond, &b->state_lock, &deadline);
        if (r == EOWNERDEAD) {            /* recupera el mutex y sigue */
            pthread_mutex_consistent(&b->state_lock);
            atomic_fetch_add(&b->owner_died, 1);
        } else if (r == ETIMEDOUT) {
            break;
        } else if (r != 0) {
            break;
        }
    }

    memcpy(out->occupant, b->occupant, sizeof(b->occupant));
    memcpy(out->dog_alive, b->dog_alive, sizeof(b->dog_alive));
    out->captured_dogs = b->captured_dogs;
    out->status = b->status;
    out->seq = b->seq;
    out->owner_died = atomic_load(&b->owner_died);

    unsigned long nuevos = b->seq - *last_seq;
    unsigned long first = *last_seq + 1;
    if (nuevos > EVENT_RING) {
        out->lost_events = (int)(nuevos - EVENT_RING);
        first = b->seq - EVENT_RING + 1;
    }
    for (unsigned long s = first; s <= b->seq; s++)
        out->events[out->n_events++] = b->events[(s - 1) % EVENT_RING];

    *last_seq = b->seq;
    unlock_state(b);
    return nuevos > 0;
}

/* ===================================================================== */
/*  Presentacion                                                          */
/* ===================================================================== */

const char *board_status_str(GameStatus s) {
    switch (s) {
        case GAME_ONGOING:           return "EN CURSO";
        case GAME_PUMA_WINS:         return "GANA PUMA (6 capturas)";
        case GAME_DOGS_WIN:          return "GANAN PERROS (puma acorralado)";
        case GAME_ABORTED_PUMA_WINS: return "GANA PUMA POR ABANDONO";
        case GAME_ABORTED_DOGS_WIN:  return "GANAN PERROS POR ABANDONO";
        case GAME_ABORTED_SERVER:    return "ABORTADA POR EL SERVIDOR";
    }
    return "?";
}

static char piece_char(int occ) {
    if (occ == PUMA_ID) return 'P';
    if (is_dog(occ))    return (char)('a' + occ);
    return '.';
}

/* snprintf acumulativo que nunca se pasa del buffer */
#define APPEND(...) do { \
        if (pos < len) { \
            int w_ = snprintf(buf + pos, len - pos, __VA_ARGS__); \
            if (w_ > 0) pos += (size_t)w_; \
        } \
    } while (0)

void board_format(const BoardSnapshot *s, int mesa_id, char *buf, size_t len) {
    size_t pos = 0;
    if (len == 0) return;
    buf[0] = '\0';

    if (s->lost_events > 0)
        APPEND("[MESA %d] (%d eventos sin mostrar)\n", mesa_id, s->lost_events);
    for (int i = 0; i < s->n_events; i++) {
        const Event *e = &s->events[i];
        if (e->type == EV_MOVE) {
            if (e->piece == PUMA_ID)
                APPEND("[MESA %d] #%lu MOV   Puma %d->%d\n", mesa_id, e->seq, e->from, e->to);
            else
                APPEND("[MESA %d] #%lu MOV   Perro %c %d->%d\n", mesa_id, e->seq,
                       piece_char(e->piece), e->from, e->to);
        } else if (e->type == EV_CAPTURE) {
            APPEND("[MESA %d] #%lu CAPT  Puma %d salta sobre %d (perro %c) -> %d\n",
                   mesa_id, e->seq, e->from, e->over, piece_char(e->victim), e->to);
        } else {
            APPEND("[MESA %d] #%lu FIN   %s\n", mesa_id, e->seq, board_status_str(e->result));
        }
    }

    const int *o = s->occupant;
    APPEND("=== [MESA %d] evento #%lu | capturados %d/%d | %s ===\n",
           mesa_id, s->seq, s->captured_dogs, DOGS_TO_WIN, board_status_str(s->status));
    APPEND("           (%c)              [00]\n", piece_char(o[0]));
    APPEND("          / | \\\n");
    APPEND("       (%c)--+--(%c)          [01-02]\n", piece_char(o[1]), piece_char(o[2]));
    APPEND("      / |   |   | \\\n");
    for (int row = 0; row < 5; row++) {
        int base = 3 + row * 5;
        APPEND("   (%c)-(%c)-(%c)-(%c)-(%c)      [%02d-%02d]\n",
               piece_char(o[base]), piece_char(o[base + 1]), piece_char(o[base + 2]),
               piece_char(o[base + 3]), piece_char(o[base + 4]), base, base + 4);
        if (row < 4) APPEND("    |   |   |   |   |\n");
    }
    APPEND("   P=puma  a-l=perros  .=vacio\n");
}
