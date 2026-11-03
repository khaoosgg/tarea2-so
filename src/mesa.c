/*
 * mesa.c — Proceso Mesa / Juez (Paso 6, con resiliencia del Paso 8)
 *
 * Corre DENTRO del proceso hijo que crea el matchmaker. Pasos:
 *   1. Crea el tablero en memoria compartida (con sus 28 mutex de casilla,
 *      state_lock y la variable de condicion), con la disposicion del config.
 *   2. fork() del proceso jugador Puma (1 hilo) y del proceso jugador Perros
 *      (12 hilos). Ambos heredan el tablero compartido.
 *   3. Arbitra: espera eventos con board_wait_and_snapshot (espera pasiva con
 *      timeout de 100 ms), imprime cada evento + el tablero en el log, revisa
 *      con waitpid(WNOHANG) que los jugadores sigan vivos y aplica el limite
 *      de seguridad max_moves. Las reglas (adyacencia, salto, victoria) se
 *      validan dentro de board_try_move/board_try_jump, en la misma seccion
 *      critica de cada jugada.
 *   4. Si un jugador muere a mitad de partida (senal o salida con error):
 *      declara ganador por abandono al oponente (seccion 4.1).
 *   5. SIGTERM del servidor (Ctrl+C): cierra la partida como ABORTADA.
 *   6. Espera a ambos jugadores, destruye el tablero (desbloqueando y
 *      destruyendo cualquier mutex que el proceso caido haya dejado tomado),
 *      y retorna el resultado como codigo de salida.
 */
#include "mesa.h"
#include "jugador.h"
#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MESA_ERROR 10

static volatile sig_atomic_t terminar = 0;

static void on_sigterm(int sig) {
    (void)sig;
    terminar = 1;
}

/* Prepara las senales de la mesa. El proceso viene de un fork del servidor,
 * que tiene SIGINT/SIGTERM bloqueadas (las atiende su hilo de senales). */
static void preparar_senales(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);          /* Ctrl+C lo maneja solo el servidor */
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);         /* aviso ordenado del servidor */

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

static pid_t lanzar_jugador(Board *b, bool es_puma, const JugadorParams *p) {
    pid_t pid = fork();
    if (pid != 0) return pid;              /* padre (Juez) o error (-1) */

    /* Hijo: jugador. SIGTERM vuelve al comportamiento por defecto. */
    signal(SIGTERM, SIG_DFL);
    log_close();                           /* el jugador no escribe en el log */
    int r = es_puma ? jugador_puma_run(b, p) : jugador_perros_run(b, p);
    _exit(r);
}

/* Un jugador "fallo" si murio por senal o salio con codigo distinto de 0 */
static bool salida_anomala(int st) {
    return WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0);
}

static void describir_salida(char *buf, size_t n, int st) {
    if (WIFSIGNALED(st)) snprintf(buf, n, "murio por senal %d (%s)", WTERMSIG(st), strsignal(WTERMSIG(st)));
    else if (WIFEXITED(st)) snprintf(buf, n, "salio con codigo %d", WEXITSTATUS(st));
    else snprintf(buf, n, "termino de forma desconocida");
}

static void imprimir_foto(const BoardSnapshot *s, int mesa_id) {
    static char buf[16384];
    board_format(s, mesa_id, buf, sizeof buf);
    log_block(buf, strlen(buf));
}

int mesa_run(int mesa_id, int cliente_puma, int cliente_perros, const Config *cfg) {
    preparar_senales();

    Board *b = board_create_shared(cfg->puma_start, cfg->dog_start);
    if (!b) {
        log_msg("[MESA %d] ERROR: no se pudo crear el tablero compartido\n", mesa_id);
        return MESA_ERROR;
    }

    JugadorParams pp = { mesa_id, cfg->delay_min_ms, cfg->delay_max_ms, cfg->seed, 0 };
    JugadorParams pd = pp;
    if (cfg->fail_mesa == mesa_id) {
        if (strcmp(cfg->fail_role, "puma") == 0) pp.fail_after_moves = cfg->fail_after_moves;
        else                                     pd.fail_after_moves = cfg->fail_after_moves;
    }

    log_msg("[MESA %d] INICIO: Puma = cliente %d | Perros = cliente %d%s\n",
            mesa_id, cliente_puma, cliente_perros,
            cfg->fail_mesa == mesa_id ? " | (falla inyectada en esta mesa)" : "");

    pid_t pid_puma = lanzar_jugador(b, true, &pp);
    pid_t pid_perros = (pid_puma > 0) ? lanzar_jugador(b, false, &pd) : -1;
    bool vivo_puma = pid_puma > 0, vivo_perros = pid_perros > 0;
    if (!vivo_puma || !vivo_perros) {
        log_msg("[MESA %d] ERROR: fork de jugadores fallo: %s\n", mesa_id, strerror(errno));
        board_finish(b, GAME_ABORTED_SERVER);
    }

    unsigned long last = 0;
    BoardSnapshot s;
    board_wait_and_snapshot(b, &last, 0, &s);
    imprimir_foto(&s, mesa_id);                     /* tablero inicial */

    /* ---------------- Bucle del Juez ---------------- */
    while (vivo_puma || vivo_perros) {
        if (board_wait_and_snapshot(b, &last, 100, &s))
            imprimir_foto(&s, mesa_id);

        if (terminar && board_finish(b, GAME_ABORTED_SERVER))
            log_msg("[MESA %d] Servidor pidio cerrar (SIGTERM): partida abortada\n", mesa_id);

        if (s.status == GAME_ONGOING && (int)s.seq >= cfg->max_moves &&
            board_finish(b, GAME_ABORTED_SERVER))
            log_msg("[MESA %d] Limite de seguridad: %d eventos sin ganador\n", mesa_id, cfg->max_moves);

        /* Deteccion de fallas: ¿algun jugador termino? */
        int st;
        char motivo[96];
        if (vivo_puma && waitpid(pid_puma, &st, WNOHANG) == pid_puma) {
            vivo_puma = false;
            if (board_finish(b, GAME_ABORTED_DOGS_WIN)) {    /* solo si seguia en curso */
                describir_salida(motivo, sizeof motivo, st);
                log_msg("[MESA %d] FALLA: el jugador Puma (pid %d) %s -> ganan Perros por abandono\n",
                        mesa_id, (int)pid_puma, motivo);
            } else if (salida_anomala(st)) {
                describir_salida(motivo, sizeof motivo, st);
                log_msg("[MESA %d] aviso: jugador Puma %s tras el fin\n", mesa_id, motivo);
            }
        }
        if (vivo_perros && waitpid(pid_perros, &st, WNOHANG) == pid_perros) {
            vivo_perros = false;
            if (board_finish(b, GAME_ABORTED_PUMA_WINS)) {
                describir_salida(motivo, sizeof motivo, st);
                log_msg("[MESA %d] FALLA: el jugador Perros (pid %d) %s -> gana Puma por abandono\n",
                        mesa_id, (int)pid_perros, motivo);
            } else if (salida_anomala(st)) {
                describir_salida(motivo, sizeof motivo, st);
                log_msg("[MESA %d] aviso: jugador Perros %s tras el fin\n", mesa_id, motivo);
            }
        }
    }

    /* Ultima foto por si quedaron eventos sin imprimir */
    if (board_wait_and_snapshot(b, &last, 0, &s)) imprimir_foto(&s, mesa_id);
    GameStatus final = s.status;

    /* Liberar: desbloquea y destruye todo lo que haya quedado tomado */
    int recuperados_en_juego = s.owner_died;
    int recuperados_al_final = board_destroy_shared(b);
    if (recuperados_en_juego > 0 || recuperados_al_final > 0)
        log_msg("[MESA %d] Recursos del proceso caido: %d lock(s) recuperados en juego, "
                "%d desbloqueados y destruidos al cerrar\n",
                mesa_id, recuperados_en_juego, recuperados_al_final < 0 ? 0 : recuperados_al_final);
    if (recuperados_al_final < 0)
        log_msg("[MESA %d] ERROR: quedaron locks tomados por un proceso vivo\n", mesa_id);

    log_msg("[MESA %d] FIN: %s (capturados %d, %lu eventos)\n",
            mesa_id, board_status_str(final), s.captured_dogs, s.seq);
    return (final >= GAME_PUMA_WINS && final <= GAME_ABORTED_SERVER) ? (int)final : MESA_ERROR;
}
