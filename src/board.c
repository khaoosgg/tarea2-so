#include "board.h"
#include <stdio.h>
#include <string.h>

static int neighbors[NUM_NODES][NUM_DIRS];
static bool graph_initialized = false;

static void init_graph_topology(void) {
    if (graph_initialized) return;
    for (int i = 0; i < NUM_NODES; i++) {
        for (int d = 0; d < NUM_DIRS; d++) {
            neighbors[i][d] = -1;
        }
    }

    neighbors[0][DIR_DOWN_LEFT] = 1;
    neighbors[1][DIR_UP_RIGHT]  = 0;
    neighbors[1][DIR_DOWN_LEFT] = 3;
    neighbors[3][DIR_UP_RIGHT]  = 1;

    neighbors[0][DIR_DOWN_RIGHT] = 2;
    neighbors[2][DIR_UP_LEFT]    = 0;
    neighbors[2][DIR_DOWN_RIGHT] = 7;
    neighbors[7][DIR_UP_LEFT]    = 2;

    neighbors[1][DIR_RIGHT] = 2;
    neighbors[2][DIR_LEFT]  = 1;

    neighbors[1][DIR_DOWN] = 4;
    neighbors[4][DIR_UP]   = 1;
    neighbors[0][DIR_DOWN] = 5;
    neighbors[5][DIR_UP]   = 0;
    neighbors[2][DIR_DOWN] = 6;
    neighbors[6][DIR_UP]   = 2;

    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            int curr = 3 + row * 5 + col;
            if (col > 0)     neighbors[curr][DIR_LEFT]  = curr - 1;
            if (col < 4)     neighbors[curr][DIR_RIGHT] = curr + 1;
            if (row > 0)     neighbors[curr][DIR_UP]    = curr - 5;
            if (row < 4)     neighbors[curr][DIR_DOWN]  = curr + 5;
        }
    }
    graph_initialized = true;
}

int board_get_neighbor(int node, Direction dir) {
    if (node < 0 || node >= NUM_NODES || dir < 0 || dir >= NUM_DIRS) return -1;
    return neighbors[node][dir];
}

void board_init(Board* b) {
    init_graph_topology();
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->judge_cond, NULL);

    for (int i = 0; i < NUM_NODES; i++) {
        b->cells[i] = CELL_EMPTY;
    }
    b->puma_pos = 0;
    b->cells[0] = CELL_PUMA;

    for (int i = 0; i < NUM_DOGS; i++) {
        int pos = 3 + i;
        b->dog_pos[i] = pos;
        b->dog_alive[i] = true;
        b->cells[pos] = CELL_DOG;
    }
    b->captured_dogs = 0;
    b->status = GAME_ONGOING;
    b->move_pending = false;
    snprintf(b->last_event, sizeof(b->last_event), "Inicio de Partida");
}

void board_destroy(Board* b) {
    pthread_cond_destroy(&b->judge_cond);
    pthread_mutex_destroy(&b->lock);
}

bool board_move_Step(Board* b, int from, int to) {
    if (from < 0 || from >= NUM_NODES || to < 0 || to >= NUM_NODES) return false;
    if (b->cells[from] == CELL_EMPTY || b->cells[to] != CELL_EMPTY) return false;

    bool adjacent = false;
    for (int d = 0; d < NUM_DIRS; d++) {
        if (neighbors[from][d] == to) {
            adjacent = true;
            break;
        }
    }
    if (!adjacent) return false;

    CellState piece = b->cells[from];
    b->cells[from] = CELL_EMPTY;
    b->cells[to] = piece;

    if (piece == CELL_PUMA) {
        b->puma_pos = to;
    } else if (piece == CELL_DOG) {
        for (int i = 0; i < NUM_DOGS; i++) {
            if (b->dog_alive[i] && b->dog_pos[i] == from) {
                b->dog_pos[i] = to;
                break;
            }
        }
    }
    return true;
}

bool board_puma_jump(Board* b, int from, int over_dog_pos, int to) {
    if (from != b->puma_pos || b->cells[from] != CELL_PUMA) return false;
    if (b->cells[over_dog_pos] != CELL_DOG || b->cells[to] != CELL_EMPTY) return false;

    bool valid_line = false;
    for (int d = 0; d < NUM_DIRS; d++) {
        if (neighbors[from][d] == over_dog_pos && neighbors[over_dog_pos][d] == to) {
            valid_line = true;
            break;
        }
    }
    if (!valid_line) return false;

    b->cells[from] = CELL_EMPTY;
    b->cells[over_dog_pos] = CELL_EMPTY;
    b->cells[to] = CELL_PUMA;
    b->puma_pos = to;

    for (int i = 0; i < NUM_DOGS; i++) {
        if (b->dog_alive[i] && b->dog_pos[i] == over_dog_pos) {
            b->dog_alive[i] = false;
            b->dog_pos[i] = -1;
            b->captured_dogs++;
            break;
        }
    }
    return true;
}

bool board_is_puma_trapped(Board* b) {
    int p = b->puma_pos;
    for (int d = 0; d < NUM_DIRS; d++) {
        int adj = neighbors[p][d];
        if (adj != -1) {
            if (b->cells[adj] == CELL_EMPTY) return false;
            if (b->cells[adj] == CELL_DOG) {
                int jump_dest = neighbors[adj][d];
                if (jump_dest != -1 && b->cells[jump_dest] == CELL_EMPTY) return false;
            }
        }
    }
    return true;
}

GameStatus board_check_victory(Board* b) {
    if (b->status != GAME_ONGOING) return b->status;
    if (b->captured_dogs >= DOGS_TO_WIN) {
        b->status = GAME_PUMA_WINS;
    } else if (board_is_puma_trapped(b)) {
        b->status = GAME_DOGS_WIN;
    }
    return b->status;
}

static char cell_char(CellState s) {
    if (s == CELL_PUMA) return 'P';
    if (s == CELL_DOG)  return 'D';
    return '.';
}

void board_print(Board* b, int mesa_id, const char* last_event) {
    printf("\n=== [MESA %d] %s | Perros capturados: %d/%d ===\n",
           mesa_id, last_event ? last_event : "Estado", b->captured_dogs, DOGS_TO_WIN);
    printf("          (%c)          [00]\n", cell_char(b->cells[0]));
    printf("         / | \\ \n");
    printf("      (%c)--+--(%c)      [01-02]\n", cell_char(b->cells[1]), cell_char(b->cells[2]));
    printf("     / |   |   | \\ \n");
    for (int row = 0; row < 5; row++) {
        int base = 3 + row * 5;
        printf("   (%c)-(%c)-(%c)-(%c)-(%c)  [%02d-%02d]\n",
               cell_char(b->cells[base]),
               cell_char(b->cells[base + 1]),
               cell_char(b->cells[base + 2]),
               cell_char(b->cells[base + 3]),
               cell_char(b->cells[base + 4]),
               base, base + 4);
    }
}
