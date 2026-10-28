// Tarea 2 Sistemas Operativos - Torneo de Komikan concurrente
// compilar: gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    Partida p;
    p.num = 1;
    p.mesa = 0;
    p.capturas = 0;
    p.movs = 0;

    for (int i = 0; i < FILAS; i++)
        for (int j = 0; j < COLS; j++)
            p.tab[i][j] = VACIO;

    // por ahora las posiciones iniciales van fijas (despues vienen de la config)
    p.tab[2][2] = 0;  // el puma
    for (int j = 0; j < COLS; j++) {
        p.tab[5][j] = j + 1;
        p.tab[6][j] = j + 6;
    }
    p.tab[4][0] = 11;
    p.tab[4][4] = 12;

    imprimir(stdout, &p);

    // pruebo un par de conexiones
    printf("(2,2)-(1,2): %d\n", conectadas(2, 2, 1, 2));
    printf("(2,2)-(3,3): %d\n", conectadas(2, 2, 3, 3));
    printf("(2,3)-(3,4): %d\n", conectadas(2, 3, 3, 4));
    return 0;
}
