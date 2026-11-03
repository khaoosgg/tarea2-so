#ifndef LOG_H
#define LOG_H

/*
 * log.h — Salida a consola y a archivo de log sin mezclar lineas.
 *
 * Cada llamada arma el mensaje completo en un buffer local y lo escribe con
 * UN solo write(). El archivo se abre con O_APPEND: en Linux cada write con
 * O_APPEND se agrega de forma atomica, aunque escriban varios procesos (mesas)
 * a la vez. No usa printf/stdio, que no es seguro despues de fork() en un
 * programa con varios hilos.
 */

#include <stddef.h>

/* Abre el log (path vacio o NULL = solo consola). Lo trunca. 0 ok, -1 error. */
int  log_open(const char *path);
/* Cierra el descriptor del archivo de log (si habia). */
void log_close(void);
/* printf hacia consola + archivo, en una sola escritura atomica. */
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Escribe un bloque ya formateado (ej.: el dibujo del tablero). */
void log_block(const char *buf, size_t len);

#endif
