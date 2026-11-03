/*
 * test_board.c — Pruebas del Paso 4 (tablero compartido + mutex por casilla)
 *
 *   make test        -> todas las pruebas
 *   make test-tsan   -> las mismas con ThreadSanitizer (detecta data races)
 *   ./test_board estres 2000   -> solo el estres con hilos, 2000 partidas
 *
 * Las fichas de estas pruebas juegan al azar con retardos de microsegundos
 * (no los 50-300 ms reales) para provocar MUCHAS carreras por segundo.
 */
#include "board.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int fallas = 0;
#define CHECK(cond, ...) do { \
        if (!(cond)) { fallas++; printf("  FALLA (%s:%d): ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* Estadisticas de carreras (atomicas: las tocan muchos hilos) */
static atomic_long st_ok, st_taken, st_not_mine, st_over, st_error;

static void count(MoveResult r) {
    switch (r) {
        case MOVE_OK:        atomic_fetch_add(&st_ok, 1); break;
        case MOVE_TAKEN:     atomic_fetch_add(&st_taken, 1); break;
        case MOVE_NOT_MINE:  atomic_fetch_add(&st_not_mine, 1); break;
        case MOVE_GAME_OVER: atomic_fetch_add(&st_over, 1); break;
        default:             atomic_fetch_add(&st_error, 1); break;
    }
}

static void default_dogs(int dogs[NUM_DOGS]) {
    for (int i = 0; i < NUM_DOGS; i++) dogs[i] = 16 + i;   /* 16..27, abajo */
}

/* ------------------------------------------------------------------ */
/*  Fichas de prueba (juego al azar)                                   */
/* ------------------------------------------------------------------ */

typedef struct { Board *b; int id; int pos; unsigned seed; } Ficha;

static void tiny_sleep(unsigned *seed) { usleep(rand_r(seed) % 300); }

static void *dog_loop(void *arg) {
    Ficha *f = arg;
    for (;;) {
        int occ[NUM_NODES]; GameStatus st;
        board_peek(f->b, occ, &st);
        if (st != GAME_ONGOING || occ[f->pos] != f->id) break;

        int opts[NUM_DIRS], n = 0;
        for (int d = 0; d < NUM_DIRS; d++) {
            int v = board_get_neighbor(f->pos, (Direction)d);
            if (v != -1 && occ[v] == NO_PIECE) opts[n++] = v;
        }
        if (n > 0) {
            int to = opts[rand_r(&f->seed) % n];
            MoveResult r = board_try_move(f->b, f->id, f->pos, to);
            count(r);
            if (r == MOVE_OK) f->pos = to;
            else if (r != MOVE_TAKEN) break;       /* capturado, fin o error */
        }
        tiny_sleep(&f->seed);
    }
    return NULL;
}

static void *puma_loop(void *arg) {
    Ficha *f = arg;
    for (;;) {
        int occ[NUM_NODES]; GameStatus st;
        board_peek(f->b, occ, &st);
        if (st != GAME_ONGOING) break;

        int jumps_over[NUM_DIRS], jumps_to[NUM_DIRS], nj = 0;
        int steps[NUM_DIRS], ns = 0;
        for (int d = 0; d < NUM_DIRS; d++) {
            int v = board_get_neighbor(f->pos, (Direction)d);
            if (v == -1) continue;
            if (occ[v] == NO_PIECE) steps[ns++] = v;
            else if (occ[v] != PUMA_ID) {
                int land = board_get_neighbor(v, (Direction)d);
                if (land != -1 && occ[land] == NO_PIECE) {
                    jumps_over[nj] = v; jumps_to[nj] = land; nj++;
                }
            }
        }
        MoveResult r = MOVE_TAKEN; int to = -1;
        if (nj > 0) {                               /* prefiere capturar */
            int k = rand_r(&f->seed) % nj;
            to = jumps_to[k];
            r = board_try_jump(f->b, f->pos, jumps_over[k], to);
        } else if (ns > 0) {
            to = steps[rand_r(&f->seed) % ns];
            r = board_try_move(f->b, PUMA_ID, f->pos, to);
        }
        if (nj > 0 || ns > 0) {
            count(r);
            if (r == MOVE_OK) f->pos = to;
            else if (r != MOVE_TAKEN) break;
        }
        tiny_sleep(&f->seed);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Juez de prueba: valida invariantes en cada foto                    */
/* ------------------------------------------------------------------ */

static bool invariantes_ok(const BoardSnapshot *s) {
    int pumas = 0, apariciones[NUM_DOGS] = { 0 }, vivos = 0;
    for (int i = 0; i < NUM_NODES; i++) {
        int o = s->occupant[i];
        if (o == PUMA_ID) pumas++;
        else if (o >= 0 && o < NUM_DOGS) apariciones[o]++;
        else if (o != NO_PIECE) return false;
    }
    if (pumas != 1) return false;
    for (int d = 0; d < NUM_DOGS; d++) {
        if (s->dog_alive[d]) { vivos++; if (apariciones[d] != 1) return false; }
        else if (apariciones[d] != 0) return false;
    }
    if (s->captured_dogs != NUM_DOGS - vivos) return false;
    if (s->status == GAME_PUMA_WINS && s->captured_dogs < DOGS_TO_WIN) return false;
    if (s->status == GAME_DOGS_WIN && board_puma_has_moves(s->occupant)) return false;
    return true;
}

/* Retorna el resultado final; *malas = fotos que rompieron invariantes */
static GameStatus juez_loop(Board *b, int limite_eventos, int *malas, bool imprimir) {
    unsigned long last = 0;
    BoardSnapshot s;
    char buf[8192];
    for (;;) {
        board_wait_and_snapshot(b, &last, 200, &s);
        if (!invariantes_ok(&s)) (*malas)++;
        if (imprimir) { board_format(&s, 1, buf, sizeof buf); fputs(buf, stdout); }
        if (s.status != GAME_ONGOING) return s.status;
        if ((int)s.seq > limite_eventos) board_finish(b, GAME_ABORTED_SERVER);
    }
}

/* ------------------------------------------------------------------ */
/*  1. Topologia                                                       */
/* ------------------------------------------------------------------ */

static void test_topologia(void) {
    printf("[1] Topologia\n");
    int aristas = 0;
    for (int i = 0; i < NUM_NODES; i++)
        for (int d = 0; d < NUM_DIRS; d++) {
            int n = board_get_neighbor(i, (Direction)d);
            if (n < 0) continue;
            aristas++;
            CHECK(board_are_adjacent(n, i), "arista %d-%d no es simetrica", i, n);
        }
    CHECK(aristas / 2 == 48, "se esperaban 48 aristas, hay %d", aristas / 2);

    int saltos[][3] = { {0,5,10}, {0,1,3}, {0,2,7}, {1,4,9}, {2,6,11},
                        {10,5,0}, {3,1,0}, {7,2,0}, {9,4,1}, {11,6,2},
                        {3,4,5}, {3,8,13}, {27,26,25} };
    for (size_t k = 0; k < sizeof saltos / sizeof saltos[0]; k++)
        CHECK(board_jump_landing(saltos[k][0], saltos[k][1]) == saltos[k][2],
              "salto %d sobre %d deberia caer en %d", saltos[k][0], saltos[k][1], saltos[k][2]);
    CHECK(board_jump_landing(1, 2) == -1, "1 sobre 2 no tiene destino");
    CHECK(board_jump_landing(5, 0) == -1, "5 sobre 0 no tiene destino");
    CHECK(!board_are_adjacent(1, 5) && !board_are_adjacent(4, 0), "diagonales falsas");
}

/* ------------------------------------------------------------------ */
/*  2. Reglas en un solo hilo                                          */
/* ------------------------------------------------------------------ */

static void test_reglas(void) {
    printf("[2] Reglas basicas\n");
    int dogs[NUM_DOGS];
    default_dogs(dogs);
    dogs[0] = 5;                               /* perro a justo bajo el trono */
    Board *b = board_create_shared(0, dogs);
    CHECK(b != NULL, "create fallo");
    if (!b) return;

    CHECK(board_try_move(b, PUMA_ID, 0, 5) == MOVE_TAKEN, "mover a casilla ocupada");
    CHECK(board_try_move(b, PUMA_ID, 0, 10) == MOVE_INVALID, "mover a no adyacente");
    CHECK(board_try_move(b, 3, 0, 1) == MOVE_NOT_MINE, "mover ficha ajena");
    CHECK(board_try_jump(b, 0, 1, 3) == MOVE_TAKEN, "saltar sobre casilla vacia");
    CHECK(board_try_jump(b, 0, 5, 15) == MOVE_INVALID, "salto que no es linea recta");
    CHECK(board_try_jump(b, 0, 5, 10) == MOVE_OK, "captura valida 0 sobre 5 a 10");
    CHECK(board_try_move(b, 0, 5, 4) == MOVE_NOT_MINE, "perro capturado no puede moverse");

    int occ[NUM_NODES]; GameStatus st;
    board_peek(b, occ, &st);
    CHECK(occ[10] == PUMA_ID && occ[5] == NO_PIECE && occ[0] == NO_PIECE, "tablero tras captura");

    CHECK(board_create_shared(0, (int[NUM_DOGS]){0,1,2,3,4,5,6,7,8,9,10,11}) == NULL,
          "debe rechazar perro encima del puma");

    CHECK(board_finish(b, GAME_ABORTED_SERVER), "finish en curso");
    CHECK(!board_finish(b, GAME_PUMA_WINS), "finish dos veces");
    CHECK(board_try_move(b, PUMA_ID, 10, 15) == MOVE_GAME_OVER, "mover tras el fin");
    CHECK(board_destroy_shared(b) == 0, "destroy limpio");

    /* Puma acorralado en la esquina 27 */
    int d2[NUM_DOGS] = { 26, 22, 25, 17, 3, 4, 5, 6, 7, 8, 9, 10 };
    b = board_create_shared(27, d2);
    CHECK(b && board_get_status(b) == GAME_DOGS_WIN, "esquina 27 rodeada = perros ganan");
    board_destroy_shared(b);
}

/* ------------------------------------------------------------------ */
/*  3. Estres con hilos (1 puma + 12 perros por partida)               */
/* ------------------------------------------------------------------ */

static void test_estres_hilos(int partidas) {
    printf("[3] Estres con hilos: %d partidas\n", partidas);
    int res[6] = { 0 }, malas = 0;
    for (int g = 0; g < partidas; g++) {
        int dogs[NUM_DOGS];
        default_dogs(dogs);
        Board *b = board_create_shared(0, dogs);
        Ficha f[NUM_DOGS + 1];
        pthread_t th[NUM_DOGS + 1];
        for (int i = 0; i < NUM_DOGS; i++) {
            f[i] = (Ficha){ b, i, dogs[i], 1000u * g + i };
            pthread_create(&th[i], NULL, dog_loop, &f[i]);
        }
        f[NUM_DOGS] = (Ficha){ b, PUMA_ID, 0, 1000u * g + 99 };
        pthread_create(&th[NUM_DOGS], NULL, puma_loop, &f[NUM_DOGS]);

        GameStatus r = juez_loop(b, 4000, &malas, false);
        res[r]++;
        for (int i = 0; i <= NUM_DOGS; i++) pthread_join(th[i], NULL);
        CHECK(board_destroy_shared(b) == 0, "partida %d: quedaron locks tomados", g);
    }
    CHECK(malas == 0, "%d fotos rompieron invariantes", malas);
    printf("    resultados: puma=%d perros=%d limite=%d | fotos invalidas=%d\n",
           res[GAME_PUMA_WINS], res[GAME_DOGS_WIN], res[GAME_ABORTED_SERVER], malas);
    printf("    jugadas: ok=%ld carreras_perdidas=%ld capturado=%ld fin=%ld error=%ld\n",
           atomic_load(&st_ok), atomic_load(&st_taken), atomic_load(&st_not_mine),
           atomic_load(&st_over), atomic_load(&st_error));
    CHECK(atomic_load(&st_error) == 0, "hubo MOVE_ERROR sin procesos muertos");
}

/* ------------------------------------------------------------------ */
/*  4. Multiproceso: Juez (padre) + proceso Puma + proceso Perros       */
/* ------------------------------------------------------------------ */

static void jugador_perros(Board *b, const int dogs[NUM_DOGS], unsigned seed) {
    Ficha f[NUM_DOGS];
    pthread_t th[NUM_DOGS];
    for (int i = 0; i < NUM_DOGS; i++) {
        f[i] = (Ficha){ b, i, dogs[i], seed + i };
        pthread_create(&th[i], NULL, dog_loop, &f[i]);
    }
    for (int i = 0; i < NUM_DOGS; i++) pthread_join(th[i], NULL);
}

static void test_multiproceso(int partidas, bool mostrar_una) {
    printf("[4] Multiproceso (memoria compartida entre procesos): %d partidas\n", partidas);
    int malas = 0, terminadas = 0;
    for (int g = 0; g < partidas; g++) {
        int dogs[NUM_DOGS];
        default_dogs(dogs);
        Board *b = board_create_shared(0, dogs);

        pid_t puma = fork();
        if (puma == 0) { Ficha f = { b, PUMA_ID, 0, 7u + g }; puma_loop(&f); _exit(0); }
        pid_t perros = fork();
        if (perros == 0) { jugador_perros(b, dogs, 100u * g); _exit(0); }

        bool imprimir = mostrar_una && g == 0;
        GameStatus r = juez_loop(b, 4000, &malas, imprimir);
        int st1, st2;
        waitpid(puma, &st1, 0);
        waitpid(perros, &st2, 0);
        CHECK(WIFEXITED(st1) && WIFEXITED(st2), "un jugador no termino bien");
        if (r != GAME_ONGOING) terminadas++;
        CHECK(board_destroy_shared(b) == 0, "quedaron locks tomados");
    }
    CHECK(malas == 0, "%d fotos invalidas", malas);
    printf("    partidas terminadas=%d | fotos invalidas=%d\n", terminadas, malas);
}

/* ------------------------------------------------------------------ */
/*  5. Robustez: un proceso muere con locks tomados                     */
/* ------------------------------------------------------------------ */

static void test_robustez(void) {
    printf("[5] Robustez (EOWNERDEAD)\n");
    int dogs[NUM_DOGS];
    default_dogs(dogs);

    /* 5a: muere con cell_lock[16] tomado -> la siguiente jugada lo recupera */
    Board *b = board_create_shared(0, dogs);
    pid_t p = fork();
    if (p == 0) { pthread_mutex_lock(&b->cell_lock[16]); kill(getpid(), SIGKILL); _exit(1); }
    int st; waitpid(p, &st, 0);
    CHECK(WIFSIGNALED(st), "el hijo debia morir por senal");
    CHECK(board_try_move(b, 0, 16, 11) == MOVE_ERROR, "primera jugada detecta dueno muerto");
    CHECK(atomic_load(&b->owner_died) == 1, "owner_died deberia ser 1");
    CHECK(board_try_move(b, 0, 16, 11) == MOVE_OK, "el lock quedo usable de nuevo");
    CHECK(board_destroy_shared(b) == 0, "destroy limpio tras recuperar");

    /* 5b: muere con una casilla Y state_lock tomados -> destroy los libera */
    b = board_create_shared(0, dogs);
    p = fork();
    if (p == 0) {
        pthread_mutex_lock(&b->cell_lock[7]);
        pthread_mutex_lock(&b->state_lock);
        kill(getpid(), SIGKILL); _exit(1);
    }
    waitpid(p, &st, 0);
    int rec = board_destroy_shared(b);
    CHECK(rec == 2, "destroy debia recuperar 2 locks, recupero %d", rec);

    /* 5c: muere con state_lock tomado -> el Juez igual puede tomar la foto */
    b = board_create_shared(0, dogs);
    p = fork();
    if (p == 0) { pthread_mutex_lock(&b->state_lock); kill(getpid(), SIGKILL); _exit(1); }
    waitpid(p, &st, 0);
    unsigned long last = 0; BoardSnapshot s;
    board_wait_and_snapshot(b, &last, 50, &s);
    CHECK(s.owner_died == 1, "el Juez debia registrar el dueno muerto");
    CHECK(board_finish(b, GAME_ABORTED_DOGS_WIN), "el Juez puede declarar abandono");
    CHECK(board_destroy_shared(b) == 0, "destroy limpio");
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && strcmp(argv[1], "estres") == 0) {
        test_estres_hilos(argc >= 3 ? atoi(argv[2]) : 500);
    } else if (argc >= 2 && strcmp(argv[1], "mostrar") == 0) {
        test_multiproceso(1, true);
    } else {
        test_topologia();
        test_reglas();
        test_estres_hilos(300);
        test_multiproceso(50, false);
        test_robustez();
    }
    printf(fallas ? "\nRESULTADO: %d FALLAS\n" : "\nRESULTADO: todo OK\n", fallas);
    return fallas ? 1 : 0;
}
