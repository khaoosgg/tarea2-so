# Tarea 2: Torneo de Komikan Concurrente
Sistemas Operativos — UDP

Servidor de emparejamiento + mesas de juego concurrentes + árbitros, en C (POSIX).
Cada mesa es un **proceso** (Juez); el Puma y los Perros son **procesos** hijos de la mesa,
con sus fichas como **hilos** (1 hilo Puma, 12 hilos Perros). El tablero vive en
**memoria compartida** (`mmap MAP_SHARED|MAP_ANONYMOUS`) y está protegido por **un mutex
por casilla** + un mutex de estado, todos `PTHREAD_PROCESS_SHARED` y `PTHREAD_MUTEX_ROBUST`.

## Compilar

```bash
make            # genera ./torneo  (gcc -Wall -Wextra -std=c17 -pthread)
make clean      # borra binarios y logs
```

## Ejecutar

```bash
./torneo N M [config.txt]
```

- `N`: número total de clientes (jugadores) a procesar (N ≥ 2). Los clientes pares juegan
  de Puma y los impares de Perros. Si sobra alguien, queda "sin rival".
- `M`: máximo de mesas activas al mismo tiempo (M ≥ 1). Los pares sobrantes esperan
  bloqueados en un semáforo (`sem_t`) hasta que se libere una mesa.
- `config.txt` (opcional): parámetros de la simulación. Sin archivo se usan los valores por defecto.

Ejemplos:

```bash
./torneo 6 2 config.txt                    # torneo normal, log en torneo.log
./torneo 4 2 configs/falla_perros.txt      # en la mesa 1 se cae el jugador Perros
./torneo 4 2 configs/falla_puma.txt        # en la mesa 2 se cae el jugador Puma
./torneo 200 8 configs/estres.txt          # 100 partidas, retardos de 0-2 ms
```

**Ctrl+C** durante el torneo: el servidor deja de aceptar jugadores, avisa a cada mesa
activa (SIGTERM) para que cierre su partida de forma ordenada, espera a todos los procesos,
libera todos los recursos, imprime el resumen y termina con código 130.

## Archivo de configuración

Formato `clave = valor`, una por línea; `#` inicia un comentario.

| Clave | Defecto | Significado |
|---|---|---|
| `delay_min_ms`, `delay_max_ms` | 50, 300 | Retardo aleatorio entre jugadas de cada ficha (ms) |
| `seed` | 42 | Semilla base. Cada ficha y cada cliente derivan su propia semilla (`rand_r`) |
| `puma_start` | 0 | Nodo inicial del Puma (0 = trono) |
| `dog_start` | 16,…,27 | 12 nodos iniciales de los Perros, separados por coma |
| `max_moves` | 2000 | Límite de seguridad: eventos máximos por partida antes de abortarla |
| `arrival_min_ms`, `arrival_max_ms` | 0, 200 | Retardo aleatorio de llegada de cada cliente |
| `log_file` | (vacío) | Archivo de log; vacío = solo consola |
| `fail_mesa` | -1 | Mesa donde se inyecta una falla (-1 = ninguna) |
| `fail_role` | perros | Jugador que falla: `puma` o `perros` |
| `fail_after_moves` | 10 | La falla ocurre tras N jugadas: el proceso muere con `SIGKILL` **teniendo un mutex tomado** |

Numeración del tablero (28 nodos):

```
            0 (trono)
        1         2
    3   4   5   6   7
    8   9  10  11  12
   13  14  15  16  17
   18  19  20  21  22
   23  24  25  26  27
```

Conexiones del triángulo: 0–1, 0–2, 0–5, 1–2, 1–3, 1–4, 2–6, 2–7. La grilla es solo ortogonal.

## Pruebas

```bash
make test        # test_board (tablero, estrés, multiproceso, robustez) + test_jugador
make test-tsan   # test_board con ThreadSanitizer (detecta data races)
./test_board mostrar      # una partida dibujada
./test_jugador mostrar    # una partida con retardos reales
```

Verificación de recursos (Ctrl+C):

```bash
valgrind --leak-check=full --track-fds=yes ./torneo 20 2 config.txt   # presionar Ctrl+C
ls /dev/shm      # no quedan bloques de memoria compartida (mmap anónimo)
ipcs             # no quedan recursos System V
```

Nota: valgrind sobre el proceso servidor reporta 0 bytes en uso y solo los 3 descriptores
estándar. Si se usa `--trace-children=yes`, los procesos hijos (creados con `fork` desde un
servidor con varios hilos) muestran como "possibly lost" las pilas de los hilos del padre que
quedaron copiadas en su memoria; el kernel las libera al terminar el hijo (`_exit`).

## Estructura

| Archivo | Contenido |
|---|---|
| `src/main.c` | Argumentos, configuración y arranque del servidor |
| `src/config.c/.h` | Lector y validación de `config.txt` |
| `src/matchmaker.c/.h` | Clientes, emparejamiento, semáforo de M mesas, cosechador, Ctrl+C |
| `src/mesa.c/.h` | Proceso Juez: crea el tablero, lanza jugadores, log, fallas, limpieza |
| `src/jugador.c/.h` | Procesos Puma (1 hilo) y Perros (12 hilos) con su estrategia |
| `src/board.c/.h` | Tablero compartido, topología, reglas y sincronización |
| `src/log.c/.h` | Salida atómica a consola y archivo (un `write` por bloque) |
| `tests/` | Pruebas automáticas |
| `configs/` | Configuraciones de ejemplo (fallas, estrés, inválida) |
