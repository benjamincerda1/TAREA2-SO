CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread

all: torneo

torneo: torneo.c
	$(CC) $(CFLAGS) torneo.c -o torneo

clean:
	rm -f torneo torneo.log salida_*.txt

.PHONY: all clean
