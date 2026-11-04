/* Habilita las extensiones POSIX/BSD (mmap anonimo, mutex robustos, rand_r...)
 * incluso si se compila con 'gcc -std=c17' sin -D. Debe ir antes de todo #include. */
#define _DEFAULT_SOURCE

/*
 * config.c — Lector de config.txt (Paso 2). Contrato en config.h.
 */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->delay_min_ms = 50;
    c->delay_max_ms = 300;
    c->seed = 42;
    c->puma_start = 0;
    for (int i = 0; i < NUM_DOGS; i++) c->dog_start[i] = 16 + i;
    c->max_moves = 2000;
    c->arrival_min_ms = 0;
    c->arrival_max_ms = 200;
    c->log_file[0] = '\0';
    c->fail_mesa = -1;
    snprintf(c->fail_role, sizeof c->fail_role, "perros");
    c->fail_after_moves = 10;
}

/* Quita espacios al inicio y al final (modifica el string) */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

/* Convierte texto a long; falla si sobra basura o se sale de [lo, hi] */
static int parse_long(const char *txt, long lo, long hi, long *out) {
    char *end;
    errno = 0;
    long v = strtol(txt, &end, 10);
    if (errno != 0 || end == txt || *trim(end) != '\0' || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

/* "16,17,...,27": exactamente 12 numeros; rechaza vacios como "16,,17" o "16,17," */
static int parse_dogs(char *txt, int dogs[NUM_DOGS]) {
    int n = 0;
    char *p = txt;
    for (;;) {
        char *coma = strchr(p, ',');
        if (coma) *coma = '\0';
        long v;
        if (n >= NUM_DOGS || parse_long(trim(p), 0, NUM_NODES - 1, &v) != 0) return -1;
        dogs[n++] = (int)v;
        if (!coma) break;
        p = coma + 1;
    }
    return n == NUM_DOGS ? 0 : -1;
}

static int validar(const Config *c, const char *path) {
    const char *err = NULL;
    if (c->delay_min_ms > c->delay_max_ms)           err = "delay_min_ms > delay_max_ms";
    else if (c->arrival_min_ms > c->arrival_max_ms)  err = "arrival_min_ms > arrival_max_ms";
    else if (strcmp(c->fail_role, "puma") != 0 && strcmp(c->fail_role, "perros") != 0)
        err = "fail_role debe ser 'puma' o 'perros'";
    else {
        bool usado[NUM_NODES] = { false };
        usado[c->puma_start] = true;
        for (int i = 0; i < NUM_DOGS && !err; i++) {
            if (usado[c->dog_start[i]]) err = "posiciones iniciales repetidas";
            usado[c->dog_start[i]] = true;
        }
    }
    if (err) {
        fprintf(stderr, "[CONFIG] %s: %s\n", path, err);
        return -1;
    }
    return 0;
}

int config_load(const char *path, Config *c) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[CONFIG] No se pudo abrir '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char linea[512];
    int nlinea = 0, errores = 0;
    while (fgets(linea, sizeof linea, f)) {
        nlinea++;
        char *hash = strchr(linea, '#');
        if (hash) *hash = '\0';
        char *s = trim(linea);
        if (*s == '\0') continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "[CONFIG] %s:%d: falta '=' en \"%s\"\n", path, nlinea, s);
            errores++;
            continue;
        }
        *eq = '\0';
        char *clave = trim(s), *valor = trim(eq + 1);
        if (*clave == '\0') {                  /* linea incompleta: "= 5" */
            fprintf(stderr, "[CONFIG] %s:%d: falta la clave antes de '='\n", path, nlinea);
            errores++;
            continue;
        }
        char original[512];                     /* para el mensaje de error */
        snprintf(original, sizeof original, "%s", valor);
        long v;
        int ok = 0;

        if      (!strcmp(clave, "delay_min_ms"))     { ok = !parse_long(valor, 0, 60000, &v); if (ok) c->delay_min_ms = (int)v; }
        else if (!strcmp(clave, "delay_max_ms"))     { ok = !parse_long(valor, 0, 60000, &v); if (ok) c->delay_max_ms = (int)v; }
        else if (!strcmp(clave, "seed"))             { ok = !parse_long(valor, 0, UINT_MAX > LONG_MAX ? LONG_MAX : (long)UINT_MAX, &v); if (ok) c->seed = (unsigned)v; }
        else if (!strcmp(clave, "puma_start"))       { ok = !parse_long(valor, 0, NUM_NODES - 1, &v); if (ok) c->puma_start = (int)v; }
        else if (!strcmp(clave, "dog_start"))        { int tmp[NUM_DOGS]; ok = !parse_dogs(valor, tmp); if (ok) memcpy(c->dog_start, tmp, sizeof tmp); }
        else if (!strcmp(clave, "max_moves"))        { ok = !parse_long(valor, 1, 10000000, &v); if (ok) c->max_moves = (int)v; }
        else if (!strcmp(clave, "arrival_min_ms"))   { ok = !parse_long(valor, 0, 60000, &v); if (ok) c->arrival_min_ms = (int)v; }
        else if (!strcmp(clave, "arrival_max_ms"))   { ok = !parse_long(valor, 0, 60000, &v); if (ok) c->arrival_max_ms = (int)v; }
        else if (!strcmp(clave, "log_file"))         { ok = strlen(valor) < CONFIG_PATH_MAX; if (ok) snprintf(c->log_file, sizeof c->log_file, "%s", valor); }
        else if (!strcmp(clave, "fail_mesa"))        { ok = !parse_long(valor, -1, 1000000, &v); if (ok) c->fail_mesa = (int)v; }
        else if (!strcmp(clave, "fail_role"))        { ok = strlen(valor) < sizeof c->fail_role; if (ok) snprintf(c->fail_role, sizeof c->fail_role, "%s", valor); }
        else if (!strcmp(clave, "fail_after_moves")) { ok = !parse_long(valor, 1, 1000000, &v); if (ok) c->fail_after_moves = (int)v; }
        else {
            fprintf(stderr, "[CONFIG] %s:%d: clave desconocida '%s' (se ignora)\n", path, nlinea, clave);
            continue;
        }
        if (!ok) {
            fprintf(stderr, "[CONFIG] %s:%d: valor invalido para '%s': \"%s\"\n", path, nlinea, clave, original);
            errores++;
        }
    }
    fclose(f);
    if (errores) return -1;
    return validar(c, path);
}

void config_print(const Config *c, FILE *out) {
    fprintf(out, "[CONFIG] retardo=%d-%d ms | seed=%u | max_moves=%d | llegada=%d-%d ms\n",
            c->delay_min_ms, c->delay_max_ms, c->seed, c->max_moves,
            c->arrival_min_ms, c->arrival_max_ms);
    fprintf(out, "[CONFIG] puma_start=%d | dog_start=", c->puma_start);
    for (int i = 0; i < NUM_DOGS; i++) fprintf(out, "%d%s", c->dog_start[i], i < NUM_DOGS - 1 ? "," : "\n");
    fprintf(out, "[CONFIG] log_file=%s | ", c->log_file[0] ? c->log_file : "(solo consola)");
    if (c->fail_mesa >= 1)
        fprintf(out, "falla inyectada: mesa %d, %s tras %d jugadas\n",
                c->fail_mesa, c->fail_role, c->fail_after_moves);
    else
        fprintf(out, "sin fallas inyectadas\n");
}
