CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -D_DEFAULT_SOURCE -pthread
LDFLAGS = -pthread

SRCS = $(wildcard src/*.c)
TARGET = torneo

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o *.log
