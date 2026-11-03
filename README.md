# Torneo de Komikan Concurrente

Tarea 2 - Sistemas Operativos. Hecho en C con pthreads (mutex y semaforos POSIX).

Integrantes: Ailyn Machuca y Benjamin Cerda

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
Al final se imprime el maximo de mesas que jugaron a la vez, para verificar que nunca se pasa de M.

Si `N` o `M` no son enteros positivos, o la config tiene errores (perros fuera del tablero,
menos de 12 perros, archivo que no existe), el programa avisa por stderr y sale con codigo 1.

## Pruebas

```
make test
```
Corre `pruebas.sh`, que prueba: torneo normal, numero impar de jugadores, caidas de fichas con y
sin el mutex tomado (con `config_fallas.txt`, compara caidas simuladas con detectadas), Ctrl+C
mientras llegan jugadores, Ctrl+C cuando solo quedan partidas jugandose, que nunca haya mas de M
mesas a la vez, que los tableros no salgan cortados en consola con muchas mesas, argumentos y
configs malas, y valgrind normal y con Ctrl+C (si esta instalado). Si algo se cuelga, el script lo
mata y lo marca como MAL. Sale con codigo 0 solo si todo esta ok.

## Probar a mano los casos especiales

- **Fallos / abandono:** en `config.txt` poner `prob_falla 5`. Alguna ficha se muere de golpe y el juez
  declara ganador por abandono al otro jugador. Con `modo_falla 0` muere con el mutex tomado (se detecta
  por el mutex robusto), con `modo_falla 1` lo suelta antes de morir (el juez la detecta porque deja de
  dar latido) y con `modo_falla 2` es al azar.
- **Ctrl+C:** correr por ejemplo `./torneo 20 2` y apretar Ctrl+C. Se cancelan las partidas, los que llegan despues son rechazados y se libera todo.
- **Memory leaks:** `valgrind --leak-check=full ./torneo 6 2 config.txt`

## Archivos

- `torneo.c` - todo el programa
- `config.txt` - configuracion de ejemplo
- `config_fallas.txt` - igual pero con `prob_falla 20` para probar las caidas
- `pruebas.sh` - script de pruebas
- `Makefile`
- `informe.pdf` - informe de diseño (2 paginas)
