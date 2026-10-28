// Tarea 2 Sistemas Operativos - Torneo de Komikan concurrente
// compilar: gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FILAS 7
#define COLS 5
#define VACIO -1

/*
 tablero de 7x5, las filas 0 y 1 son el triangulo (trono) y
 de la 2 a la 6 es el cuadrado de 5x5:

     o   o   o        fila 0
       o o o          fila 1
     o o o o o        fila 2  (la punta del trono es la 2,2)
     ...
     o o o o o        fila 6
*/

// configuracion (se puede cambiar con config.txt)
int rmin = 50, rmax = 300;
unsigned int semilla;
int max_movs = 200;
int puma_ini[2] = {2, 2};
int perros_ini[12][2] = {
    {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
    {6, 0}, {6, 1}, {6, 2}, {6, 3}, {6, 4},
    {4, 0}, {4, 4}
};
int nperros = 12;

typedef struct {
    int num;
    int mesa;
    int tab[FILAS][COLS];
    int capturas;
    int movs;
} Partida;

int existe(int f, int c)
{
    if (f < 0 || f >= FILAS || c < 0 || c >= COLS) return 0;
    if (f == 0) return c == 0 || c == 2 || c == 4;
    if (f == 1) return c >= 1 && c <= 3;
    return 1;
}

// las lineas del trono son pocas asi que las puse a mano
int trono[10][4] = {
    {2,2,1,1}, {2,2,1,2}, {2,2,1,3},
    {1,1,0,0}, {1,2,0,2}, {1,3,0,4},
    {1,1,1,2}, {1,2,1,3},
    {0,0,0,2}, {0,2,0,4}
};

int conectadas(int f1, int c1, int f2, int c2)
{
    if (!existe(f1, c1) || !existe(f2, c2)) return 0;

    if (f1 >= 2 && f2 >= 2) {
        int df = abs(f1 - f2), dc = abs(c1 - c2);
        if (df > 1 || dc > 1) return 0;
        if (df == 0 && dc == 0) return 0;
        if (df == 1 && dc == 1)
            return (f1 + c1) % 2 == 0;  // solo hay diagonales en los puntos "pares"
        return 1;
    }

    for (int i = 0; i < 10; i++) {
        if (trono[i][0] == f1 && trono[i][1] == c1 && trono[i][2] == f2 && trono[i][3] == c2) return 1;
        if (trono[i][0] == f2 && trono[i][1] == c2 && trono[i][2] == f1 && trono[i][3] == c1) return 1;
    }
    return 0;
}

void imprimir(FILE *out, Partida *p)
{
    fprintf(out, "  [mesa %d] partida %d - movimientos: %d - capturas: %d\n", p->mesa + 1, p->num, p->movs, p->capturas);
    for (int i = 0; i < FILAS; i++) {
        fprintf(out, "    ");
        for (int j = 0; j < COLS; j++) {
            if (!existe(i, j)) fprintf(out, "  ");
            else if (p->tab[i][j] == VACIO) fprintf(out, ". ");
            else if (p->tab[i][j] == 0) fprintf(out, "P ");
            else fprintf(out, "o ");
        }
        fprintf(out, "\n");
    }
    fprintf(out, "\n");
}

void leer_config(char *archivo)
{
    FILE *f = fopen(archivo, "r");
    if (f == NULL) {
        printf("no pude abrir %s, uso los valores por defecto\n", archivo);
        return;
    }
    char linea[200], nombre[50];
    int a, b;
    int perros_leidos = 0;

    while (fgets(linea, sizeof(linea), f)) {
        if (linea[0] == '#') continue;
        int n = sscanf(linea, "%49s %d %d", nombre, &a, &b);
        if (n < 2) continue;

        if (strcmp(nombre, "retardo_min") == 0) rmin = a;
        else if (strcmp(nombre, "retardo_max") == 0) rmax = a;
        else if (strcmp(nombre, "semilla") == 0) semilla = a;
        else if (strcmp(nombre, "max_movimientos") == 0) max_movs = a;
        else if (strcmp(nombre, "puma") == 0 && n == 3) {
            puma_ini[0] = a;
            puma_ini[1] = b;
        } else if (strcmp(nombre, "perro") == 0 && n == 3 && perros_leidos < 12) {
            perros_ini[perros_leidos][0] = a;
            perros_ini[perros_leidos][1] = b;
            perros_leidos++;
            nperros = perros_leidos;
        }
    }
    fclose(f);
}

int config_ok(void)
{
    if (rmin < 0 || rmax < rmin || max_movs <= 0) {
        printf("error: revisar retardos o max_movimientos en la config\n");
        return 0;
    }
    int usado[FILAS][COLS] = {{0}};
    if (!existe(puma_ini[0], puma_ini[1])) {
        printf("error: el puma esta fuera del tablero\n");
        return 0;
    }
    usado[puma_ini[0]][puma_ini[1]] = 1;
    for (int i = 0; i < nperros; i++) {
        int f = perros_ini[i][0], c = perros_ini[i][1];
        if (!existe(f, c) || usado[f][c]) {
            printf("error: el perro %d esta mal puesto (%d,%d)\n", i + 1, f, c);
            return 0;
        }
        usado[f][c] = 1;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    semilla = time(NULL);
    if (argc > 1) leer_config(argv[1]);
    if (!config_ok()) return 1;

    printf("retardo %d-%d ms, semilla %u, max %d movimientos\n\n", rmin, rmax, semilla, max_movs);

    Partida p;
    p.num = 1;
    p.mesa = 0;
    p.capturas = 0;
    p.movs = 0;

    for (int i = 0; i < FILAS; i++)
        for (int j = 0; j < COLS; j++)
            p.tab[i][j] = VACIO;

    // ahora las posiciones iniciales salen de la config
    p.tab[puma_ini[0]][puma_ini[1]] = 0;
    for (int i = 0; i < nperros; i++)
        p.tab[perros_ini[i][0]][perros_ini[i][1]] = i + 1;

    imprimir(stdout, &p);
    return 0;
}
