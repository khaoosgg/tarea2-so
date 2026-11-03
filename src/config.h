#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h — CONTRATO del lector de configuracion (Paso 2)
 *
 * Este .h ya esta fijo para que todos programemos contra lo mismo.
 * La implementacion (src/config.c) la escribe el compañero.
 *
 * Formato de config.txt (una opcion por linea, "clave = valor"):
 *
 *     # comentario: se ignora todo lo que va despues de '#'
 *     delay_min_ms   = 50
 *     delay_max_ms   = 300
 *     seed           = 42
 *     puma_start     = 0
 *     dog_start      = 16,17,18,19,20,21,22,23,24,25,26,27
 *     max_moves      = 2000
 *     arrival_min_ms = 0
 *     arrival_max_ms = 200
 *     log_file       = torneo.log
 *     fail_mesa      = -1
 *     fail_role      = perros
 *     fail_after_moves = 10
 *
 * Reglas:
 *   - Espacios alrededor de '=' y lineas vacias se ignoran.
 *   - Clave desconocida -> aviso por stderr, se ignora (no es error fatal).
 *   - Valor invalido -> mensaje por stderr y config_load retorna -1.
 *   - Lo que no aparezca en el archivo queda con su valor por defecto.
 *   - Si no se pasa config.txt al programa, se usan solo los valores por defecto.
 */

#include <stdio.h>
#include "board.h"   /* NUM_DOGS, NUM_NODES */

#define CONFIG_PATH_MAX 256

typedef struct {
    /* Retardo entre jugadas de cada ficha, en ms (enunciado: 50 a 300) */
    int delay_min_ms;            /* defecto 50  */
    int delay_max_ms;            /* defecto 300 */

    /* Semilla para reproducibilidad */
    unsigned seed;               /* defecto 42 */

    /* Disposicion inicial: nodos 0..27, todos distintos */
    int puma_start;              /* defecto 0 (trono) */
    int dog_start[NUM_DOGS];     /* defecto 16..27 (las dos filas de abajo + 16,17) */

    /* Limite de seguridad: si una partida supera esta cantidad de eventos,
     * el Juez la cierra como GAME_ABORTED_SERVER (evita partidas infinitas) */
    int max_moves;               /* defecto 2000 */

    /* Llegada de clientes al servidor: retardo aleatorio entre llegadas (ms) */
    int arrival_min_ms;          /* defecto 0   */
    int arrival_max_ms;          /* defecto 200 */

    /* Archivo de log. Cadena vacia = solo consola */
    char log_file[CONFIG_PATH_MAX];  /* defecto "" */

    /* Falla inyectada para demostrar resiliencia (seccion 4.1):
     *   fail_mesa = -1 desactiva. Si es >= 1, en esa mesa el jugador fail_role
     *   ("puma" o "perros") se cae tras fail_after_moves jugadas propias. */
    int  fail_mesa;              /* defecto -1 */
    char fail_role[8];           /* defecto "perros" */
    int  fail_after_moves;       /* defecto 10 */
} Config;

/* Llena c con los valores por defecto. */
void config_defaults(Config *c);

/* Carga el archivo 'path' ENCIMA de lo que ya tenga c (llamar antes a
 * config_defaults). Retorna 0 si todo bien, -1 si no se pudo abrir el archivo
 * o algun valor es invalido (ya imprimio el motivo por stderr).
 * Validaciones minimas:
 *   0 <= delay_min_ms <= delay_max_ms; 0 <= arrival_min_ms <= arrival_max_ms;
 *   max_moves > 0; fail_after_moves > 0; fail_role es "puma" o "perros";
 *   puma_start y los 12 dog_start en 0..27 y sin repetirse;
 *   dog_start trae exactamente 12 numeros. */
int config_load(const char *path, Config *c);

/* Imprime la configuracion efectiva (util para el log y para depurar). */
void config_print(const Config *c, FILE *out);

#endif
