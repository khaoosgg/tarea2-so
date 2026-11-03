#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

static void* puma_routine(void* arg) {
    PieceThreadArgs* args = (PieceThreadArgs*)arg;
    Board* b = args->board;

    unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)pthread_self());

    while (true) {
        int delay_ms = 50 + (rand_r(&seed) % 251);
        usleep(delay_ms * 1000);

        pthread_mutex_lock(&b->lock);

        if (b->status != GAME_ONGOING) {
            pthread_mutex_unlock(&b->lock);
            break;
        }

        int p = b->puma_pos;
        bool moved = false;
        int start_d = rand_r(&seed) % NUM_DIRS;

        for (int i = 0; i < NUM_DIRS; i++) {
            int d = (start_d + i) % NUM_DIRS;
            int adj = board_get_neighbor(p, d);
            if (adj != -1 && b->cells[adj] == CELL_DOG) {
                int jump_dest = board_get_neighbor(adj, d);
                if (jump_dest != -1 && b->cells[jump_dest] == CELL_EMPTY) {
                    if (board_puma_jump(b, p, adj, jump_dest)) {
                        snprintf(b->last_event, sizeof(b->last_event),
                                 "Puma SALTA a [%02d] (captura en [%02d])", jump_dest, adj);
                        moved = true;
                        break;
                    }
                }
            }
        }

        if (!moved) {
            for (int i = 0; i < NUM_DIRS; i++) {
                int d = (start_d + i) % NUM_DIRS;
                int adj = board_get_neighbor(p, d);
                if (adj != -1 && b->cells[adj] == CELL_EMPTY) {
                    if (board_move_Step(b, p, adj)) {
                        snprintf(b->last_event, sizeof(b->last_event),
                                 "Puma se mueve a [%02d]", adj);
                        moved = true;
                        break;
                    }
                }
            }
        }

        board_check_victory(b);
        if (moved || b->status != GAME_ONGOING) {
            if (moved) b->move_pending = true;
            pthread_cond_signal(&b->judge_cond);
        }

        pthread_mutex_unlock(&b->lock);
    }
    return NULL;
}

static void* dog_routine(void* arg) {
    PieceThreadArgs* args = (PieceThreadArgs*)arg;
    Board* b = args->board;
    int dog_idx = args->piece_id;

    unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)pthread_self() ^ (dog_idx * 97));

    while (true) {
        int delay_ms = 50 + (rand_r(&seed) % 251);
        usleep(delay_ms * 1000);

        pthread_mutex_lock(&b->lock);

        if (b->status != GAME_ONGOING || !b->dog_alive[dog_idx]) {
            pthread_mutex_unlock(&b->lock);
            break;
        }

        int curr_pos = b->dog_pos[dog_idx];
        int start_d = rand_r(&seed) % NUM_DIRS;

        for (int i = 0; i < NUM_DIRS; i++) {
            int d = (start_d + i) % NUM_DIRS;
            int adj = board_get_neighbor(curr_pos, d);
            if (adj != -1 && b->cells[adj] == CELL_EMPTY) {
                if (board_move_Step(b, curr_pos, adj)) {
                    board_check_victory(b);
                    if (b->status != GAME_ONGOING) {
                        snprintf(b->last_event, sizeof(b->last_event),
                                 "Perro %02d acorrala al Puma desde [%02d]", dog_idx, adj);
                        b->move_pending = true;
                        pthread_cond_signal(&b->judge_cond);
                    }
                    break;
                }
            }
        }

        pthread_mutex_unlock(&b->lock);
    }
    return NULL;
}

static void* judge_routine(void* arg) {
    PieceThreadArgs* args = (PieceThreadArgs*)arg;
    Board* b = args->board;
    int mesa_id = args->mesa_id;

    pthread_mutex_lock(&b->lock);
    board_print(b, mesa_id, "Inicio de Partida");

    while (b->status == GAME_ONGOING) {
        while (!b->move_pending && b->status == GAME_ONGOING) {
            pthread_cond_wait(&b->judge_cond, &b->lock);
        }

        if (b->move_pending) {
            board_print(b, mesa_id, b->last_event);
            b->move_pending = false;
        }
        board_check_victory(b);
    }

    pthread_mutex_unlock(&b->lock);
    return NULL;
}

void run_table_simulation(int mesa_id) {
    Board board;
    board_init(&board);

    printf("\n[MESA %d] ¡Comienza la partida de Komikan Concurrente en tiempo real!\n", mesa_id);

    pthread_t judge_thread;
    pthread_t puma_thread;
    pthread_t dog_threads[NUM_DOGS];

    PieceThreadArgs judge_args = { &board, mesa_id, -2, false };
    PieceThreadArgs puma_args  = { &board, mesa_id, -1, true };
    PieceThreadArgs dog_args[NUM_DOGS];

    pthread_create(&judge_thread, NULL, judge_routine, &judge_args);
    pthread_create(&puma_thread, NULL, puma_routine, &puma_args);
    for (int i = 0; i < NUM_DOGS; i++) {
        dog_args[i] = (PieceThreadArgs){ &board, mesa_id, i, false };
        pthread_create(&dog_threads[i], NULL, dog_routine, &dog_args[i]);
    }

    pthread_join(judge_thread, NULL);
    pthread_join(puma_thread, NULL);
    for (int i = 0; i < NUM_DOGS; i++) {
        pthread_join(dog_threads[i], NULL);
    }

    printf("\n====================================================\n");
    if (board.status == GAME_PUMA_WINS) {
        printf("[MESA %d] ¡FIN DEL JUEGO! El PUMA ha ganado capturando a %d perros.\n", mesa_id, board.captured_dogs);
    } else {
        printf("[MESA %d] ¡FIN DEL JUEGO! Los PERROS han acorralado al Puma en [%02d].\n", mesa_id, board.puma_pos);
    }
    printf("====================================================\n");

    board_destroy(&board);
}
