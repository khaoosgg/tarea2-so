/*
 * test_jugador.c — Pruebas del Paso 5 (fichas reales como hilos en procesos)
 *
 *   make test                     -> corre test_board y test_jugador
 *   ./test_jugador partidas 200   -> solo el balance de partidas
 *   ./test_jugador mostrar        -> una partida con retardos reales (50-300 ms)
 *
 * Aqui el "Juez" es una version minima (la real llega en el Paso 6): espera
 * eventos, revisa que los jugadores sigan vivos con waitpid(WNOHANG) y, si uno
 * muere, declara ganador por abandono al oponente.
 */
#include "board.h"
#include "jugador.h"

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

typedef struct {
    GameStatus resultado;
    unsigned long eventos;
    int recuperados;        /* locks recuperados al destruir el tablero */
    bool puma_murio, perros_murio;
} Partida;

static void dogs_default(int d[NUM_DOGS]) { for (int i = 0; i < NUM_DOGS; i++) d[i] = 16 + i; }

static Partida jugar(int mesa, unsigned seed, int dmin, int dmax, int limite,
                     int falla_puma, int falla_perros, bool imprimir) {
    Partida out = { 0 };
    int dogs[NUM_DOGS];
    dogs_default(dogs);
    Board *b = board_create_shared(0, dogs);

    JugadorParams pp = { mesa, dmin, dmax, seed, falla_puma };
    JugadorParams pd = { mesa, dmin, dmax, seed, falla_perros };

    pid_t puma = fork();
    if (puma == 0) _exit(jugador_puma_run(b, &pp));
    pid_t perros = fork();
    if (perros == 0) _exit(jugador_perros_run(b, &pd));

    bool vivo_puma = true, vivo_perros = true;
    unsigned long last = 0;
    BoardSnapshot s;
    char buf[8192];
    for (;;) {
        board_wait_and_snapshot(b, &last, 100, &s);
        if (imprimir && s.n_events > 0) { board_format(&s, mesa, buf, sizeof buf); fputs(buf, stdout); }

        int st;
        if (vivo_puma && waitpid(puma, &st, WNOHANG) == puma) {
            vivo_puma = false;
            if (s.status == GAME_ONGOING && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
                out.puma_murio = true;
                board_finish(b, GAME_ABORTED_DOGS_WIN);
            }
        }
        if (vivo_perros && waitpid(perros, &st, WNOHANG) == perros) {
            vivo_perros = false;
            if (s.status == GAME_ONGOING && !(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
                out.perros_murio = true;
                board_finish(b, GAME_ABORTED_PUMA_WINS);
            }
        }
        if ((int)s.seq > limite) board_finish(b, GAME_ABORTED_SERVER);

        GameStatus actual = board_get_status(b);
        if (actual != GAME_ONGOING) { out.resultado = actual; break; }
    }
    if (vivo_puma) waitpid(puma, NULL, 0);
    if (vivo_perros) waitpid(perros, NULL, 0);

    board_wait_and_snapshot(b, &last, 0, &s);
    if (imprimir && s.n_events > 0) { board_format(&s, mesa, buf, sizeof buf); fputs(buf, stdout); }
    out.eventos = s.seq;
    int al_destruir = board_destroy_shared(b);
    /* recuperados durante la partida (EOWNERDEAD) + los que seguian tomados */
    out.recuperados = (al_destruir < 0) ? -1 : s.owner_died + al_destruir;
    return out;
}

static void test_distancias(void) {
    printf("[1] Distancias en el grafo\n");
    CHECK(jugador_distancia(0, 0) == 0, "d(0,0)");
    CHECK(jugador_distancia(0, 5) == 1, "d(0,5)");
    CHECK(jugador_distancia(0, 25) == 5, "d(0,25) = 0->5->10->15->20->25");
    CHECK(jugador_distancia(1, 3) == 1, "d(1,3) diagonal");
    CHECK(jugador_distancia(23, 27) == 4, "d(23,27)");
    for (int a = 0; a < NUM_NODES; a++)
        for (int c = 0; c < NUM_NODES; c++)
            CHECK(jugador_distancia(a, c) == jugador_distancia(c, a), "simetria %d %d", a, c);
}

static void test_balance(int n) {
    printf("[2] Balance con estrategia: %d partidas (retardos 0-1 ms)\n", n);
    int res[6] = { 0 }, sucias = 0;
    unsigned long eventos = 0;
    for (int g = 0; g < n; g++) {
        Partida p = jugar(1, 1000u + g, 0, 1, 3000, 0, 0, false);
        res[p.resultado]++;
        eventos += p.eventos;
        if (p.recuperados != 0) sucias++;
    }
    printf("    puma=%d perros=%d limite=%d | promedio %lu eventos/partida\n",
           res[GAME_PUMA_WINS], res[GAME_DOGS_WIN], res[GAME_ABORTED_SERVER], eventos / (unsigned long)n);
    CHECK(sucias == 0, "%d partidas dejaron locks tomados", sucias);
    CHECK(res[GAME_PUMA_WINS] + res[GAME_DOGS_WIN] + res[GAME_ABORTED_SERVER] == n,
          "resultados que no corresponden");
}

static void test_fallas(void) {
    printf("[3] Fallas a proposito (Paso 8 adelantado)\n");
    Partida p = jugar(1, 7, 0, 1, 3000, 0, 5, false);
    CHECK(p.perros_murio, "el proceso Perros debia morir");
    CHECK(p.resultado == GAME_ABORTED_PUMA_WINS, "debia ganar el puma por abandono (%d)", p.resultado);
    CHECK(p.recuperados >= 1, "debia recuperarse 1 lock tomado, se recuperaron %d", p.recuperados);
    printf("    Perros cae: %s, locks recuperados=%d\n", board_status_str(p.resultado), p.recuperados);

    p = jugar(1, 8, 0, 1, 3000, 3, 0, false);
    CHECK(p.puma_murio, "el proceso Puma debia morir");
    CHECK(p.resultado == GAME_ABORTED_DOGS_WIN, "debian ganar los perros por abandono (%d)", p.resultado);
    CHECK(p.recuperados >= 1, "debia recuperarse 1 lock, se recuperaron %d", p.recuperados);
    printf("    Puma cae:   %s, locks recuperados=%d\n", board_status_str(p.resultado), p.recuperados);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 2 && strcmp(argv[1], "mostrar") == 0) {
        Partida p = jugar(1, 42, 50, 300, 3000, 0, 0, true);
        printf("\nResultado: %s en %lu eventos\n", board_status_str(p.resultado), p.eventos);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "partidas") == 0) {
        test_balance(argc >= 3 ? atoi(argv[2]) : 100);
    } else {
        test_distancias();
        test_balance(100);
        test_fallas();
    }
    printf(fallas ? "\nRESULTADO: %d FALLAS\n" : "\nRESULTADO: todo OK\n", fallas);
    return fallas ? 1 : 0;
}
