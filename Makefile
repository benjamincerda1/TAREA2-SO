CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -pthread

all: torneo

torneo: torneo.c
	$(CC) $(CFLAGS) torneo.c -o torneo

clean:
	rm -f torneo torneo.log salida_*.txt config_rapida.txt config_lento.txt config_estres.txt config_mala.txt config_pocos.txt

.PHONY: all clean test

test: torneo
	sh pruebas.sh
