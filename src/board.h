#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <pthread.h>

#define NUM_NODES 28
#define NUM_DOGS 12
#define DOGS_TO_WIN 6

typedef enum {
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT,
    DIR_UP_LEFT,
    DIR_UP_RIGHT,
    DIR_DOWN_LEFT,
    DIR_DOWN_RIGHT,
    NUM_DIRS
} Direction;

typedef enum {
    CELL_EMPTY = 0,
    CELL_PUMA,
    CELL_DOG
} CellState;

typedef enum {
    GAME_ONGOING = 0,
    GAME_PUMA_WINS,
    GAME_DOGS_WIN,
    GAME_ABORTED_PUMA_WINS,
    GAME_ABORTED_DOGS_WIN
} GameStatus;

typedef struct {
    CellState cells[NUM_NODES];
    int puma_pos;
    int dog_pos[NUM_DOGS];
    bool dog_alive[NUM_DOGS];
    int captured_dogs;
    GameStatus status;
    pthread_mutex_t lock;
} Board;

void board_init(Board* b);
void board_destroy(Board* b);
bool board_move_Step(Board* b, int from, int to);
bool board_puma_jump(Board* b, int from, int over_dog_pos, int to);
bool board_is_puma_trapped(Board* b);
GameStatus board_check_victory(Board* b);
void board_print(Board* b, int mesa_id, const char* last_event);
int board_get_neighbor(int node, Direction dir);

#endif