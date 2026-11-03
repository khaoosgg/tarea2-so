#ifndef GAME_H
#define GAME_H

#include "board.h"

// Estructura para pasar los argumentos a los hilos de las fichas
typedef struct {
    Board* board;
    int mesa_id;
    int piece_id; // -1 para el Puma, 0 a 11 para los Perros
    bool is_puma;
} PieceThreadArgs;

// Ejecuta una partida completa en una mesa hasta que haya un ganador
void run_table_simulation(int mesa_id);

#endif