// Tarea 2 Sistemas Operativos - Torneo de Komikan concurrente
// compilar: gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <semaphore.h>

#define FILAS 7
#define COLS 5
#define VACIO -1

#define SIN_JUGAR 0
#define GANA_PUMA 1
#define GANAN_PERROS 2
#define EMPATE 3
#define CANCELADA 4

/*
tablero de 7x5, las filas 0 y 1 son el triangulo (trono) y
de la 2 a la 6 es el cuadrado de 5x5:

o o o fila 0
o o o fila 1
o o o o o fila 2 (la punta del trono es la 2,2)
...
o o o o o fila 6
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

typedef struct partida Partida;

typedef struct {
int id; // 0 es el puma, los demas son perros
int fila, col;
int vivo;
unsigned int seed;
pthread_t hilo;
Partida *p;
} Ficha;

struct partida {
int num;
int mesa;
int tab[FILAS][COLS];
Ficha fichas[13];
int nfichas;
int capturas;
int movs;
int terminado;
int resultado;
int abandono;
int dueno; // que ficha tiene el mutex ahora (-1 = juez o servidor)
pthread_mutex_t mutex;
sem_t fin; // el juez espera aqui hasta que se termine la partida
};

FILE *flog = NULL;
pthread_mutex_t mlog = PTHREAD_MUTEX_INITIALIZER;


void dormir(int ms)
{
struct timespec t;
t.tv_sec = ms / 1000;
t.tv_nsec = (ms % 1000) * 1000000L;
nanosleep(&t, NULL);
}

void escribir_log(const char *formato, ...)
{
if (flog == NULL) return;
va_list args;
va_start(args, formato);
pthread_mutex_lock(&mlog);
vfprintf(flog, formato, args);
fflush(flog);
pthread_mutex_unlock(&mlog);
va_end(args);
}

// sem_wait se puede cortar por la señal (EINTR), en ese caso reintento.
// cualquier otro error es un problema de verdad
void esperar(sem_t *s)
{
while (sem_wait(s) != 0) {
if (errno != EINTR) {
perror("sem_wait");
exit(1);
}
}
}

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
return (f1 + c1) % 2 == 0; // solo hay diagonales en los puntos "pares"
return 1;
}

for (int i = 0; i < 10; i++) {
if (trono[i][0] == f1 && trono[i][1] == c1 && trono[i][2] == f2 && trono[i][3] == c2) return 1;
if (trono[i][0] == f2 && trono[i][1] == c2 && trono[i][2] == f1 && trono[i][3] == c1) return 1;
}
return 0;
}

// guarda en movf/movc los movimientos posibles, devuelve cuantos hay
// salto[i] = 1 si ese movimiento es el puma comiendose a un perro
int movimientos(int tab[FILAS][COLS], int id, int f, int c, int *movf, int *movc, int *salto)
{
int n = 0;
for (int i = 0; i < FILAS; i++) {
for (int j = 0; j < COLS; j++) {
if (!conectadas(f, c, i, j)) continue;

if (tab[i][j] == VACIO) {
movf[n] = i; movc[n] = j; salto[n] = 0;
n++;
} else if (id == 0 && tab[i][j] > 0) {
int f2 = 2*i - f;
int c2 = 2*j - c;
if (conectadas(i, j, f2, c2) && tab[f2][c2] == VACIO) {
movf[n] = f2; movc[n] = c2; salto[n] = 1;
n++;
}
}
}
}
return n;
}

int elegir(int tab[FILAS][COLS], Ficha *fi, int *df, int *dc)
{
int movf[40], movc[40], salto[40];
int n = movimientos(tab, fi->id, fi->fila, fi->col, movf, movc, salto);
if (n == 0) return 0;

int k = rand_r(&fi->seed) % n;

if (fi->id == 0) {
// el puma si puede comer, come
for (int i = 0; i < n; i++)
if (salto[i]) k = i;
} else if (rand_r(&fi->seed) % 2 == 0) {
// a veces el perro se acerca al puma para encerrarlo
int pf = 0, pc = 0;
for (int i = 0; i < FILAS; i++)
for (int j = 0; j < COLS; j++)
if (tab[i][j] == 0) { pf = i; pc = j; }
int mejor = 100;
for (int i = 0; i < n; i++) {
int d = abs(movf[i] - pf) + abs(movc[i] - pc);
if (d < mejor) { mejor = d; k = i; }
}
}

*df = movf[k];
*dc = movc[k];
return 1;
}

void imprimir(FILE *out, Partida *p)
{
fprintf(out, " [mesa %d] partida %d - movimientos: %d - capturas: %d\n", p->mesa + 1, p->num, p->movs, p->capturas);
for (int i = 0; i < FILAS; i++) {
fprintf(out, " ");
for (int j = 0; j < COLS; j++) {
if (!existe(i, j)) fprintf(out, " ");
else if (p->tab[i][j] == VACIO) fprintf(out, ". ");
else if (p->tab[i][j] == 0) fprintf(out, "P ");
else fprintf(out, "o ");
}
fprintf(out, "\n");
}
fprintf(out, "\n");
}

void tablero_al_log(Partida *p)
{
if (flog == NULL) return;
pthread_mutex_lock(&mlog);
imprimir(flog, p);
fflush(flog);
pthread_mutex_unlock(&mlog);
}

// marca la partida como terminada y despierta al juez (con el mutex tomado)
void terminar(Partida *p, int resultado)
{
if (p->terminado) return;
p->terminado = 1;
p->resultado = resultado;
sem_post(&p->fin);
}

// todos toman el mutex de la partida con esta funcion
void tomar(Partida *p, int quien)
{
pthread_mutex_lock(&p->mutex);
p->dueno = quien;
}

// esto lo ejecuta la ficha pero es "el juez" revisando el movimiento
// hay que llamarla con el mutex de la partida tomado
void mover(Partida *p, Ficha *fi, int of, int oc, int df, int dc)
{
int comido = -1, mf = 0, mc = 0;

// la ficha decidio desde (of,oc), si ya no esta ahi el movimiento no sirve
if (fi->fila != of || fi->col != oc) {
escribir_log("[mesa %d] ficha %d ya no esta en (%d,%d), movimiento rechazado\n", p->mesa + 1, fi->id, of, oc);
return;
}

if (!existe(df, dc) || p->tab[df][dc] != VACIO) {
// otra ficha le gano la casilla
escribir_log("[mesa %d] ficha %d quiso ir a (%d,%d) pero ya estaba ocupada\n", p->mesa + 1, fi->id, df, dc);
return;
}

if (!conectadas(of, oc, df, dc)) {
// si no es al lado tiene que ser un salto del puma
mf = (of + df) / 2;
mc = (oc + dc) / 2;
if (fi->id != 0 || (of + df) % 2 != 0 || (oc + dc) % 2 != 0 ||
!conectadas(of, oc, mf, mc) || !conectadas(mf, mc, df, dc) || p->tab[mf][mc] <= 0) {
escribir_log("[mesa %d] movimiento invalido de la ficha %d\n", p->mesa + 1, fi->id);
return;
}
comido = p->tab[mf][mc];
}

p->tab[of][oc] = VACIO;
p->tab[df][dc] = fi->id;
fi->fila = df;
fi->col = dc;
escribir_log("[mesa %d] ficha %d: (%d,%d) -> (%d,%d)\n", p->mesa + 1, fi->id, of, oc, df, dc);

if (comido > 0) {
p->tab[mf][mc] = VACIO;
p->fichas[comido].vivo = 0;
p->capturas++;
printf("[mesa %d] el puma se comio al perro %d (lleva %d)\n", p->mesa + 1, comido, p->capturas);
escribir_log("[mesa %d] el puma se comio al perro %d\n", p->mesa + 1, comido);
}
p->movs++;
tablero_al_log(p);

// ver si alguien gano
int movf[40], movc[40], salto[40];
Ficha *puma = &p->fichas[0];
if (p->capturas >= 6 || p->capturas == nperros)
terminar(p, GANA_PUMA);
else if (movimientos(p->tab, 0, puma->fila, puma->col, movf, movc, salto) == 0)
terminar(p, GANAN_PERROS);
else if (p->movs >= max_movs)
terminar(p, EMPATE);
}

void *ficha(void *arg)
{
Ficha *fi = arg;
Partida *p = fi->p;
int of, oc, df = 0, dc = 0;

while (1) {
tomar(p, fi->id);
if (p->terminado || !fi->vivo) {
pthread_mutex_unlock(&p->mutex);
break;
}
of = fi->fila;
oc = fi->col;
int puedo = elegir(p->tab, fi, &df, &dc);
pthread_mutex_unlock(&p->mutex);

// "pensando"... mientras tanto los demas pueden moverse
dormir(rmin + rand_r(&fi->seed) % (rmax - rmin + 1));

if (!puedo) continue;

tomar(p, fi->id);
if (p->terminado || !fi->vivo) {
pthread_mutex_unlock(&p->mutex);
break;
}

mover(p, fi, of, oc, df, dc);
pthread_mutex_unlock(&p->mutex);
}
return NULL;
}

void *juez(void *arg)
{
Partida *p = arg;

// 1. primitivas y tablero
pthread_mutex_init(&p->mutex, NULL);
sem_init(&p->fin, 0, 0);

for (int i = 0; i < FILAS; i++)
for (int j = 0; j < COLS; j++)
p->tab[i][j] = VACIO;

p->nfichas = nperros + 1;
for (int i = 0; i < p->nfichas; i++) {
Ficha *fi = &p->fichas[i];
fi->id = i;
fi->vivo = 1;
fi->p = p;
fi->seed = semilla + p->num * 100 + i;
if (i == 0) {
fi->fila = puma_ini[0];
fi->col = puma_ini[1];
} else {
fi->fila = perros_ini[i-1][0];
fi->col = perros_ini[i-1][1];
}
p->tab[fi->fila][fi->col] = i;
}
p->capturas = 0;
p->movs = 0;
p->terminado = 0;
p->resultado = SIN_JUGAR;
p->abandono = 0;
p->dueno = -1;

printf("[mesa %d] empieza la partida %d\n", p->mesa + 1, p->num);
escribir_log("[mesa %d] ======= empieza partida %d =======\n", p->mesa + 1, p->num);
pthread_mutex_lock(&mlog); // para que no se mezcle con el tablero de otra mesa
imprimir(stdout, p);
pthread_mutex_unlock(&mlog);
tablero_al_log(p);

// por si la disposicion inicial de la config ya deja al puma encerrado
int movf[40], movc[40], salto[40];
if (movimientos(p->tab, 0, puma_ini[0], puma_ini[1], movf, movc, salto) == 0)
terminar(p, GANAN_PERROS);

// 2. lanzar las fichas
int creadas = 0;
for (int i = 0; i < p->nfichas; i++) {
if (pthread_create(&p->fichas[i].hilo, NULL, ficha, &p->fichas[i]) != 0) {
printf("[mesa %d] error creando el hilo de la ficha %d, se cancela la partida\n", p->mesa + 1, i);
tomar(p, -1);
terminar(p, CANCELADA);
pthread_mutex_unlock(&p->mutex);
break;
}
creadas++;
}

// 3. esperar (sin gastar cpu) a que alguien termine la partida
esperar(&p->fin);

for (int i = 0; i < creadas; i++)
pthread_join(p->fichas[i].hilo, NULL);

char *txt[] = {"sin jugar", "gana el puma", "ganan los perros", "empate", "cancelada"};
printf("[mesa %d] termino la partida %d: %s%s (%d movimientos, %d capturas)\n", p->mesa + 1, p->num,
txt[p->resultado], p->abandono ? " por abandono" : "", p->movs, p->capturas);
escribir_log("[mesa %d] ======= fin partida %d: %s%s =======\n", p->mesa + 1, p->num,
txt[p->resultado], p->abandono ? " por abandono" : "");
pthread_mutex_lock(&mlog);
imprimir(stdout, p);
pthread_mutex_unlock(&mlog);

pthread_mutex_destroy(&p->mutex);
sem_destroy(&p->fin);
return NULL;
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

flog = fopen("torneo.log", "w");
if (flog == NULL) printf("no se pudo crear torneo.log, sigo sin log\n");

printf("=== Torneo de Komikan ===\n");
printf("retardo %d-%d ms, semilla %u\n\n", rmin, rmax, semilla);

// por ahora juego una sola partida, sin jugadores ni mesas
Partida *p = malloc(sizeof(Partida));
if (p == NULL) {
printf("error: no hay memoria\n");
return 1;
}
p->num = 1;
p->mesa = 0;
juez(p);
free(p);

if (flog != NULL) fclose(flog);
printf("\nfin de la partida de prueba\n");
return 0;
}
