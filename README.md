# Torneo de Komikan Concurrente

Tarea 2 - Sistemas Operativos. Hecho en C con pthreads (mutex y semaforos POSIX).

## Compilar

```
make
```
o a mano:
```
gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo
```

## Ejecutar

```
./torneo N M [config.txt]
```
- `N`: cantidad de jugadores
- `M`: cantidad maxima de mesas jugando al mismo tiempo
- `config.txt`: opcional (retardos, semilla, posiciones iniciales, etc.)

Ejemplos:
```
./torneo 8 2
./torneo 10 3 config.txt
```

En consola salen los eventos importantes (llegadas, emparejamientos, capturas, fin de cada partida).
El tablero completo despues de cada movimiento queda en `torneo.log`.

## Pruebas

```
make test
```
Corre `pruebas.sh`, que prueba: torneo normal, numero impar de jugadores, caidas de fichas
(con `config_fallas.txt`), Ctrl+C mientras llegan jugadores, Ctrl+C cuando solo quedan partidas
jugandose y valgrind (si esta instalado). Si algo se cuelga, el script lo mata y lo marca como MAL.

## Probar a mano los casos especiales

- **Fallos / abandono:** en `config.txt` poner `prob_falla 5`. Alguna ficha se muere de golpe con el mutex
  tomado, el siguiente hilo lo detecta (mutex robusto) y el otro jugador gana por abandono.
- **Ctrl+C:** correr por ejemplo `./torneo 20 2` y apretar Ctrl+C. Se cancelan las partidas, los que llegan despues son rechazados y se libera todo.
- **Memory leaks:** `valgrind --leak-check=full ./torneo 6 2 config.txt`

## Archivos

- `torneo.c` - todo el programa
- `config.txt` - configuracion de ejemplo
- `config_fallas.txt` - igual pero con `prob_falla 20` para probar las caidas
- `pruebas.sh` - script de pruebas
- `Makefile`
- `informe.pdf` - informe de diseño (2 paginas)
