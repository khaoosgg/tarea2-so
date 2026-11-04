CC      = gcc
CFLAGS  = -Wall -Wextra -std=c17 -pthread -g
LDFLAGS = -pthread

SRCS   = $(wildcard src/*.c)
HDRS   = $(wildcard src/*.h)
TARGET = torneo

# Los tests no van en src/ porque tienen su propio main.
# Usan todos los .c de src/ menos main.c
LIB_SRCS = $(filter-out src/main.c, $(SRCS))
TESTS    = test_board test_jugador

.PHONY: all clean test test-tsan test-config

all: $(TARGET)

# Los .h son dependencia: si cambia board.h, se recompila
$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

test_%: tests/test_%.c $(LIB_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -O1 -Isrc -o $@ $< $(LIB_SRCS) $(LDFLAGS)

test: $(TESTS) $(TARGET)
	./test_board
	./test_jugador
	bash tests/test_config.sh

test-config: $(TARGET)
	bash tests/test_config.sh

# test_board con ThreadSanitizer: detecta condiciones de carrera
test-tsan: tests/test_board.c $(LIB_SRCS) $(HDRS)
	$(CC) $(CFLAGS) -O1 -fsanitize=thread -Isrc -o test_board_tsan $< $(LIB_SRCS) $(LDFLAGS)
	./test_board_tsan

clean:
	rm -f $(TARGET) $(TESTS) test_board_tsan *.o *.log
