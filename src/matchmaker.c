/* Habilita las extensiones POSIX/BSD (mmap anonimo, mutex robustos, rand_r...)
 * incluso si se compila con 'gcc -std=c17' sin -D. Debe ir antes de todo #include. */
#define _DEFAULT_SOURCE

/*
 * matchmaker.c — Servidor de emparejamiento (Paso 7) + Ctrl+C (Paso 9)
 * Ver el resumen en matchmaker.h.
 *
 * SINCRONIZACION DEL SERVIDOR
 *   - srv.mtx (pthread_mutex) protege TODO el estado del servidor: colas,
 *     estado de cada cliente, tabla de mesas activas y la bandera 'cerrando'.
 *   - srv.cv_matchmaker: el matchmaker duerme aqui hasta que haya un par
 *     completo, o lleguen todos, o se cierre el servidor.
 *   - srv.cv_clientes: los clientes duermen aqui (esperando su llegada o su
 *     asignacion a mesa). Se despiertan con broadcast al asignar o al cerrar.
 *   - srv.cv_cosechador: el cosechador duerme aqui mientras no haya mesas.
 *   - srv.mesas_libres (sem_t, valor inicial M): cupos de mesa.
 *   Orden: nunca se espera el semaforo teniendo srv.mtx tomado.
 */
#include "matchmaker.h"
#include "mesa.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

typedef enum { CLI_NO_LLEGA = 0, CLI_ESPERANDO, CLI_EN_MESA, CLI_SIN_RIVAL, CLI_CANCELADO } EstadoCliente;

typedef struct {
    int id;
    pid_t pid;
    int cliente_puma, cliente_perros;
    int resultado;                      /* WEXITSTATUS de la mesa, -1 = en curso */
    struct timespec inicio;
    double duracion_s;
} InfoMesa;

static struct {
    int N, M;
    const Config *cfg;

    pthread_mutex_t mtx;
    pthread_cond_t cv_matchmaker, cv_clientes, cv_cosechador;
    sem_t mesas_libres;

    EstadoCliente *estado;              /* por cliente */
    int *cola_puma, *cola_perros;       /* colas FIFO de ids de cliente */
    int qp_ini, qp_fin, qd_ini, qd_fin;
    int llegados, procesados;           /* procesados = llegaron o se cancelaron */

    InfoMesa *mesas;                    /* todas las mesas creadas */
    int n_mesas, activas;
    bool fin_creacion;                  /* el matchmaker ya no creara mas mesas */
    atomic_bool cerrando;               /* Ctrl+C recibido */
} srv;

static double segundos_desde(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

static struct timespec deadline_ms(clockid_t clk, int ms) {
    struct timespec t;
    clock_gettime(clk, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

/* ===================================================================== */
/*  Hilo cliente                                                          */
/* ===================================================================== */

static void *hilo_cliente(void *arg) {
    int id = (int)(intptr_t)arg;
    unsigned seed = srv.cfg->seed * 2654435761u + (unsigned)id;
    int lo = srv.cfg->arrival_min_ms, hi = srv.cfg->arrival_max_ms;
    int espera = lo + (hi > lo ? (int)(rand_r(&seed) % (unsigned)(hi - lo + 1)) : 0);

    pthread_mutex_lock(&srv.mtx);
    /* Llegada: dormir 'espera' ms, pero despertar al tiro si se cierra */
    struct timespec dl = deadline_ms(CLOCK_MONOTONIC, espera);
    while (!atomic_load(&srv.cerrando)) {
        if (pthread_cond_timedwait(&srv.cv_clientes, &srv.mtx, &dl) == ETIMEDOUT) break;
    }
    if (atomic_load(&srv.cerrando)) {
        srv.estado[id] = CLI_NO_LLEGA;           /* el servidor ya no acepta */
    } else {
        srv.estado[id] = CLI_ESPERANDO;
        srv.llegados++;
        if (id % 2 == 0) srv.cola_puma[srv.qp_fin++] = id;
        else             srv.cola_perros[srv.qd_fin++] = id;
    }
    srv.procesados++;
    pthread_cond_signal(&srv.cv_matchmaker);

    /* Esperar pasivamente la asignacion a una mesa */
    while (srv.estado[id] == CLI_ESPERANDO)
        pthread_cond_wait(&srv.cv_clientes, &srv.mtx);
    pthread_mutex_unlock(&srv.mtx);
    return NULL;
}

/* ===================================================================== */
/*  Hilo matchmaker                                                       */
/* ===================================================================== */

/* Espera un cupo de mesa sin quedarse pegado si llega Ctrl+C */
static bool esperar_cupo(void) {
    for (;;) {
        if (atomic_load(&srv.cerrando)) return false;
        struct timespec dl = deadline_ms(CLOCK_REALTIME, 100);
        if (sem_timedwait(&srv.mesas_libres, &dl) == 0) return true;
        if (errno != ETIMEDOUT && errno != EINTR) return false;
    }
}

static void *hilo_matchmaker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&srv.mtx);
        while (!atomic_load(&srv.cerrando) &&
               !(srv.qp_ini < srv.qp_fin && srv.qd_ini < srv.qd_fin) &&
               srv.procesados < srv.N)
            pthread_cond_wait(&srv.cv_matchmaker, &srv.mtx);

        bool hay_par = srv.qp_ini < srv.qp_fin && srv.qd_ini < srv.qd_fin;
        if (atomic_load(&srv.cerrando) || !hay_par) {
            pthread_mutex_unlock(&srv.mtx);
            break;                              /* no habra mas pares */
        }
        int cp = srv.cola_puma[srv.qp_ini++];
        int cd = srv.cola_perros[srv.qd_ini++];
        pthread_mutex_unlock(&srv.mtx);

        log_msg("[SERVIDOR] Par listo: cliente %d (Puma) + cliente %d (Perros); esperando mesa libre...\n", cp, cd);
        bool cupo = esperar_cupo();             /* espera pasiva: sem_t con valor M */

        pthread_mutex_lock(&srv.mtx);
        if (!cupo || atomic_load(&srv.cerrando)) {
            if (cupo) sem_post(&srv.mesas_libres);
            srv.estado[cp] = srv.estado[cd] = CLI_CANCELADO;
            pthread_cond_broadcast(&srv.cv_clientes);
            pthread_mutex_unlock(&srv.mtx);
            break;
        }
        /* fork con srv.mtx tomado: asi el hilo de senales no puede "perderse"
         * una mesa recien creada. El hijo no usa srv.mtx. */
        InfoMesa *m = &srv.mesas[srv.n_mesas];
        m->id = srv.n_mesas + 1;
        m->cliente_puma = cp;
        m->cliente_perros = cd;
        m->resultado = -1;
        clock_gettime(CLOCK_MONOTONIC, &m->inicio);
        pid_t pid = fork();
        if (pid == 0) {
            int r = mesa_run(m->id, cp, cd, srv.cfg);
            log_close();                        /* cerrar el fd heredado del log */
            _exit(r);
        }
        if (pid < 0) {
            log_msg("[SERVIDOR] ERROR: fork de mesa fallo: %s\n", strerror(errno));
            sem_post(&srv.mesas_libres);
            srv.estado[cp] = srv.estado[cd] = CLI_CANCELADO;
        } else {
            m->pid = pid;
            srv.n_mesas++;
            srv.activas++;
            srv.estado[cp] = srv.estado[cd] = CLI_EN_MESA;
            log_msg("[SERVIDOR] Mesa %d abierta (pid %d) | mesas activas: %d/%d\n",
                    m->id, (int)pid, srv.activas, srv.M);
            pthread_cond_signal(&srv.cv_cosechador);
        }
        pthread_cond_broadcast(&srv.cv_clientes);
        pthread_mutex_unlock(&srv.mtx);
    }

    /* Clientes que quedaron sin rival o sin atender */
    pthread_mutex_lock(&srv.mtx);
    EstadoCliente quedan = atomic_load(&srv.cerrando) ? CLI_CANCELADO : CLI_SIN_RIVAL;
    for (int i = srv.qp_ini; i < srv.qp_fin; i++) srv.estado[srv.cola_puma[i]] = quedan;
    for (int i = srv.qd_ini; i < srv.qd_fin; i++) srv.estado[srv.cola_perros[i]] = quedan;
    srv.qp_ini = srv.qp_fin;
    srv.qd_ini = srv.qd_fin;
    srv.fin_creacion = true;
    pthread_cond_broadcast(&srv.cv_clientes);
    pthread_cond_signal(&srv.cv_cosechador);
    pthread_mutex_unlock(&srv.mtx);
    return NULL;
}

/* ===================================================================== */
/*  Hilo cosechador                                                       */
/* ===================================================================== */

static void *hilo_cosechador(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&srv.mtx);
        while (srv.activas == 0 && !srv.fin_creacion)
            pthread_cond_wait(&srv.cv_cosechador, &srv.mtx);
        if (srv.activas == 0 && srv.fin_creacion) {
            pthread_mutex_unlock(&srv.mtx);
            break;
        }
        pthread_mutex_unlock(&srv.mtx);

        int st;
        pid_t pid = waitpid(-1, &st, 0);        /* bloquea hasta que termine una mesa */
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }

        pthread_mutex_lock(&srv.mtx);
        for (int i = 0; i < srv.n_mesas; i++) {
            InfoMesa *m = &srv.mesas[i];
            if (m->pid != pid || m->resultado != -1) continue;
            m->resultado = WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st);
            m->duracion_s = segundos_desde(&m->inicio);
            srv.activas--;
            log_msg("[SERVIDOR] Mesa %d terminada: %s (%.1f s) | mesas activas: %d/%d\n",
                    m->id,
                    (m->resultado >= 1 && m->resultado <= 5) ? board_status_str((GameStatus)m->resultado)
                                                             : "ERROR",
                    m->duracion_s, srv.activas, srv.M);
            sem_post(&srv.mesas_libres);        /* libera el cupo */
            break;
        }
        pthread_mutex_unlock(&srv.mtx);
    }
    return NULL;
}

/* ===================================================================== */
/*  Hilo de senales (Ctrl+C)                                              */
/* ===================================================================== */

static void *hilo_senales(void *arg) {
    sigset_t *set = arg;
    for (;;) {
        int sig;
        if (sigwait(set, &sig) != 0) continue;
        if (sig == SIGUSR1) break;              /* fin normal: main nos despide */

        /* SIGINT / SIGTERM */
        pthread_mutex_lock(&srv.mtx);
        if (!atomic_load(&srv.cerrando)) {
            atomic_store(&srv.cerrando, true);
            log_msg("\n[SERVIDOR] Senal %s recibida: no se aceptan mas jugadores; "
                    "cerrando %d mesa(s) activa(s)...\n", sig == SIGINT ? "SIGINT (Ctrl+C)" : "SIGTERM",
                    srv.activas);
            for (int i = 0; i < srv.n_mesas; i++)
                if (srv.mesas[i].resultado == -1) kill(srv.mesas[i].pid, SIGTERM);
            pthread_cond_broadcast(&srv.cv_clientes);
            pthread_cond_broadcast(&srv.cv_matchmaker);
            pthread_cond_signal(&srv.cv_cosechador);
        }
        pthread_mutex_unlock(&srv.mtx);
    }
    return NULL;
}

/* ===================================================================== */
/*  Punto de entrada                                                      */
/* ===================================================================== */

static const char *estado_str(EstadoCliente e) {
    switch (e) {
        case CLI_EN_MESA:   return "jugo";
        case CLI_SIN_RIVAL: return "sin rival";
        case CLI_CANCELADO: return "cancelado (servidor cerro)";
        case CLI_NO_LLEGA:  return "no alcanzo a llegar";
        default:            return "esperando";
    }
}

int matchmaker_run(int N, int M, const Config *cfg) {
    memset(&srv, 0, sizeof srv);
    srv.N = N;
    srv.M = M;
    srv.cfg = cfg;
    atomic_init(&srv.cerrando, false);

    srv.estado = calloc((size_t)N, sizeof *srv.estado);
    srv.cola_puma = calloc((size_t)N, sizeof(int));
    srv.cola_perros = calloc((size_t)N, sizeof(int));
    srv.mesas = calloc((size_t)N / 2 + 1, sizeof *srv.mesas);
    pthread_t *clientes = calloc((size_t)N, sizeof(pthread_t));
    if (!srv.estado || !srv.cola_puma || !srv.cola_perros || !srv.mesas || !clientes) {
        fprintf(stderr, "[SERVIDOR] sin memoria\n");
        free(srv.estado); free(srv.cola_puma); free(srv.cola_perros); free(srv.mesas); free(clientes);
        return 1;
    }

    pthread_mutex_init(&srv.mtx, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&srv.cv_matchmaker, &ca);
    pthread_cond_init(&srv.cv_clientes, &ca);
    pthread_cond_init(&srv.cv_cosechador, &ca);
    pthread_condattr_destroy(&ca);
    sem_init(&srv.mesas_libres, 0, (unsigned)M);

    /* Bloquear SIGINT/SIGTERM/SIGUSR1 ANTES de crear hilos: los heredan todos,
     * y solo el hilo de senales los recibe (con sigwait). */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    pthread_t th_senales, th_matchmaker, th_cosechador;
    pthread_create(&th_senales, NULL, hilo_senales, &set);
    pthread_create(&th_cosechador, NULL, hilo_cosechador, NULL);
    pthread_create(&th_matchmaker, NULL, hilo_matchmaker, NULL);
    int creados = 0;
    for (int i = 0; i < N; i++) {
        if (pthread_create(&clientes[i], NULL, hilo_cliente, (void *)(intptr_t)i) != 0) {
            pthread_mutex_lock(&srv.mtx);
            for (int k = i; k < N; k++) { srv.estado[k] = CLI_NO_LLEGA; srv.procesados++; }
            pthread_cond_signal(&srv.cv_matchmaker);
            pthread_mutex_unlock(&srv.mtx);
            log_msg("[SERVIDOR] aviso: solo se pudieron crear %d hilos cliente\n", i);
            break;
        }
        creados++;
    }

    pthread_join(th_matchmaker, NULL);
    pthread_join(th_cosechador, NULL);
    for (int i = 0; i < creados; i++) pthread_join(clientes[i], NULL);
    pthread_kill(th_senales, SIGUSR1);          /* despedir al hilo de senales */
    pthread_join(th_senales, NULL);

    bool interrumpido = atomic_load(&srv.cerrando);

    /* ---------------- Resumen ---------------- */
    int res[6] = { 0 }, errores = 0;
    log_msg("\n================ RESUMEN DEL TORNEO ================\n");
    for (int i = 0; i < srv.n_mesas; i++) {
        InfoMesa *m = &srv.mesas[i];
        bool ok = m->resultado >= 1 && m->resultado <= 5;
        if (ok) res[m->resultado]++; else errores++;
        log_msg("Mesa %2d | Puma=cliente %2d vs Perros=cliente %2d | %-32s | %.1f s\n",
                m->id, m->cliente_puma, m->cliente_perros,
                ok ? board_status_str((GameStatus)m->resultado) : "ERROR", m->duracion_s);
    }
    log_msg("Mesas: %d | Puma: %d | Perros: %d | Abandonos: %d | Abortadas: %d | Errores: %d\n",
            srv.n_mesas, res[GAME_PUMA_WINS], res[GAME_DOGS_WIN],
            res[GAME_ABORTED_PUMA_WINS] + res[GAME_ABORTED_DOGS_WIN], res[GAME_ABORTED_SERVER], errores);
    for (int i = 0; i < N; i++)
        if (srv.estado[i] != CLI_EN_MESA)
            log_msg("Cliente %2d (%s): %s\n", i, i % 2 == 0 ? "Puma" : "Perros", estado_str(srv.estado[i]));
    log_msg(interrumpido ? "Torneo INTERRUMPIDO con Ctrl+C. Recursos liberados.\n"
                         : "Torneo terminado.\n");
    log_msg("====================================================\n");

    /* ---------------- Liberar todo ---------------- */
    sem_destroy(&srv.mesas_libres);
    pthread_cond_destroy(&srv.cv_matchmaker);
    pthread_cond_destroy(&srv.cv_clientes);
    pthread_cond_destroy(&srv.cv_cosechador);
    pthread_mutex_destroy(&srv.mtx);
    free(srv.estado);
    free(srv.cola_puma);
    free(srv.cola_perros);
    free(srv.mesas);
    free(clientes);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    return interrumpido ? 130 : 0;
}
