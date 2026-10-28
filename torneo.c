// Tarea 2 Sistemas Operativos - Torneo de Komikan concurrente
// compilar: gcc -Wall -Wextra -std=c17 -pthread torneo.c -o torneo
// uso: ./torneo N M [config.txt]

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
int prob_falla = 0;  // por mil
int puma_ini[2] = {2, 2};
int perros_ini[12][2] = {
    {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
    {6, 0}, {6, 1}, {6, 2}, {6, 3}, {6, 4},
    {4, 0}, {4, 4}
};
int nperros = 12;

typedef struct partida Partida;

typedef struct {
    int id;  // 0 es el puma, los demas son perros
    int fila, col;
    int vivo;
    unsigned int seed;
    pthread_t hilo;
    Partida *p;
} Ficha;

struct partida {
    int num;
    int mesa;
    int jpuma, jperros;
    int tab[FILAS][COLS];
    Ficha fichas[13];
    int nfichas;
    int capturas;
    int movs;
    int terminado;
    int resultado;
    int abandono;
    int dueno;  // que ficha tiene el mutex ahora (-1 = juez o servidor)
    pthread_mutex_t mutex;
    sem_t fin;  // el juez espera aqui hasta que se termine la partida
};

typedef struct {
    int id;
    int rol;  // 0 puma 1 perros
    int mesa;
    int resultado;
    int abandono;
    sem_t listo;
    pthread_t hilo;
} Jugador;

int N, M;
Jugador *jug;

int *cola;
int ini = 0, fin = 0;
int llegaron = 0;
pthread_mutex_t mcola = PTHREAD_MUTEX_INITIALIZER;
sem_t avisos;  // cada vez que llega un jugador (o hay ctrl+c) se hace post

sem_t mesas_libres;
int *mesa_ocupada;
int *juez_creado;
pthread_t *jueces;
pthread_mutex_t mmesas = PTHREAD_MUTEX_INITIALIZER;

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
            return (f1 + c1) % 2 == 0;  // solo hay diagonales en los puntos "pares"
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

// todos toman el mutex de la partida con esta funcion.
// el mutex es robusto: si una ficha se muere teniendolo, el siguiente que
// lo pide recibe EOWNERDEAD en vez de quedarse bloqueado para siempre
void tomar(Partida *p, int quien)
{
    if (pthread_mutex_lock(&p->mutex) == EOWNERDEAD) {
        pthread_mutex_consistent(&p->mutex);
        printf("[mesa %d] se detecto que la ficha %d se desconecto\n", p->mesa + 1, p->dueno);
        escribir_log("[mesa %d] la ficha %d murio con el mutex tomado\n", p->mesa + 1, p->dueno);
        if (!p->terminado) {
            p->abandono = 1;
            terminar(p, p->dueno == 0 ? GANAN_PERROS : GANA_PUMA);
        }
    }
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

        // para probar las desconexiones: el hilo se muere de golpe con el mutex
        // tomado y sin avisarle a nadie
        if (prob_falla > 0 && (int)(rand_r(&fi->seed) % 1000) < prob_falla) {
            printf("[mesa %d] (prueba) se cae la ficha %d\n", p->mesa + 1, fi->id);
            pthread_exit(NULL);
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
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&p->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
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

    printf("[mesa %d] empieza la partida %d: jugador %d (puma) vs jugador %d (perros)\n", p->mesa + 1, p->num, p->jpuma, p->jperros);
    escribir_log("[mesa %d] ======= empieza partida %d =======\n", p->mesa + 1, p->num);
    pthread_mutex_lock(&mlog);  // para que no se mezcle con el tablero de otra mesa
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

    // 4. avisar a los jugadores y liberar todo
    jug[p->jpuma].resultado = p->resultado;
    jug[p->jpuma].abandono = p->abandono;
    jug[p->jperros].resultado = p->resultado;
    jug[p->jperros].abandono = p->abandono;
    sem_post(&jug[p->jpuma].listo);
    sem_post(&jug[p->jperros].listo);

    int m = p->mesa;
    pthread_mutex_lock(&mmesas);
    mesa_ocupada[m] = 0;
    pthread_mutex_unlock(&mmesas);

    pthread_mutex_destroy(&p->mutex);
    sem_destroy(&p->fin);
    free(p);

    sem_post(&mesas_libres);
    sem_post(&avisos);  // le aviso al servidor que se desocupo una mesa
    return NULL;
}

void *jugador(void *arg)
{
    Jugador *j = arg;
    unsigned int s = semilla + 999 + j->id;

    // los jugadores llegan en distintos momentos (0 a 2 s)
    dormir(rand_r(&s) % 2000);

    pthread_mutex_lock(&mcola);
    cola[fin] = j->id;
    fin++;
    llegaron++;
    printf("[jugador %d] llego, esperando rival\n", j->id);
    pthread_mutex_unlock(&mcola);
    sem_post(&avisos);

    esperar(&j->listo);

    if (j->resultado == SIN_JUGAR || j->resultado == CANCELADA) {
        printf("[jugador %d] me voy sin terminar de jugar\n", j->id);
    } else if (j->resultado == EMPATE) {
        printf("[jugador %d] empate en la mesa %d\n", j->id, j->mesa + 1);
    } else {
        int gane = (j->rol == 0 && j->resultado == GANA_PUMA) || (j->rol == 1 && j->resultado == GANAN_PERROS);
        printf("[jugador %d] jugue con %s en la mesa %d y %s%s\n", j->id, j->rol == 0 ? "el puma" : "los perros",
               j->mesa + 1, gane ? "GANE" : "perdi", j->abandono ? " (por abandono)" : "");
    }
    return NULL;
}

int mesas_en_uso(void)
{
    int n = 0;
    pthread_mutex_lock(&mmesas);
    for (int i = 0; i < M; i++)
        if (mesa_ocupada[i]) n++;
    pthread_mutex_unlock(&mmesas);
    return n;
}

// devuelve 0 si no se pudo armar la partida
int nueva_partida(int a, int b, int num)
{
    Partida *p = malloc(sizeof(Partida));
    if (p == NULL) {
        printf("[servidor] no hay memoria para otra partida\n");
        return 0;
    }

    int m = 0;
    pthread_mutex_lock(&mmesas);
    while (mesa_ocupada[m]) m++;
    mesa_ocupada[m] = 1;
    pthread_mutex_unlock(&mmesas);

    if (juez_creado[m]) {
        pthread_join(jueces[m], NULL);  // el juez anterior de esta mesa ya esta saliendo
        juez_creado[m] = 0;
    }

    p->num = num;
    p->mesa = m;
    p->jpuma = a;
    p->jperros = b;
    jug[a].rol = 0;
    jug[a].mesa = m;
    jug[b].rol = 1;
    jug[b].mesa = m;

    if (pthread_create(&jueces[m], NULL, juez, p) != 0) {
        printf("[servidor] no se pudo crear el juez de la mesa %d\n", m + 1);
        free(p);
        pthread_mutex_lock(&mmesas);
        mesa_ocupada[m] = 0;
        pthread_mutex_unlock(&mmesas);
        return 0;
    }
    juez_creado[m] = 1;
    return 1;
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
        else if (strcmp(nombre, "prob_falla") == 0) prob_falla = a;
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
    if (argc < 3) {
        printf("uso: %s N M [config.txt]\n", argv[0]);
        return 1;
    }
    N = atoi(argv[1]);
    M = atoi(argv[2]);
    if (N <= 0 || M <= 0) {
        printf("N y M tienen que ser positivos\n");
        return 1;
    }

    semilla = time(NULL);
    if (argc > 3) leer_config(argv[3]);
    if (!config_ok()) return 1;

    jug = malloc(N * sizeof(Jugador));
    cola = malloc(N * sizeof(int));
    mesa_ocupada = calloc(M, sizeof(int));
    juez_creado = calloc(M, sizeof(int));
    jueces = malloc(M * sizeof(pthread_t));
    if (!jug || !cola || !mesa_ocupada || !juez_creado || !jueces) {
        printf("error: no hay memoria\n");
        free(jug); free(cola); free(mesa_ocupada); free(juez_creado); free(jueces);
        return 1;
    }

    if (sem_init(&mesas_libres, 0, M) != 0 || sem_init(&avisos, 0, 0) != 0) {
        perror("sem_init");
        return 1;
    }

    flog = fopen("torneo.log", "w");
    if (flog == NULL) printf("no se pudo crear torneo.log, sigo sin log\n");

    printf("=== Torneo de Komikan ===\n");
    printf("%d jugadores, %d mesas, retardo %d-%d ms, semilla %u\n\n", N, M, rmin, rmax, semilla);


    for (int i = 0; i < N; i++) {
        jug[i].id = i;
        jug[i].rol = -1;
        jug[i].mesa = -1;
        jug[i].resultado = SIN_JUGAR;
        jug[i].abandono = 0;
        sem_init(&jug[i].listo, 0, 0);
        if (pthread_create(&jug[i].hilo, NULL, jugador, &jug[i]) != 0) {
            // si no se pudo crear, sigo con los que ya estan
            printf("error creando el jugador %d, el torneo sigue con %d jugadores\n", i, i);
            sem_destroy(&jug[i].listo);
            pthread_mutex_lock(&mcola);
            N = i;
            pthread_mutex_unlock(&mcola);
            break;
        }
    }

    int partidas = 0;
    while (N > 0) {
        esperar(&avisos);  // dormido hasta que llegue alguien o se desocupe una mesa

        int a = -1, b = -1, salir = 0;
        pthread_mutex_lock(&mcola);
        if (fin - ini >= 2) {
            a = cola[ini];
            b = cola[ini + 1];
            ini += 2;
        } else if (llegaron == N && fin - ini == 1) {
            printf("[servidor] el jugador %d se quedo sin rival\n", cola[ini]);
            sem_post(&jug[cola[ini]].listo);
            ini++;
        }
        if (llegaron == N && ini == fin)
            salir = 1;
        pthread_mutex_unlock(&mcola);

        if (a != -1) {
            printf("[servidor] pareja: jugador %d (puma) y jugador %d (perros), esperando mesa...\n", a, b);

            // si las M mesas estan ocupadas se queda bloqueado aqui
            esperar(&mesas_libres);
            if (nueva_partida(a, b, partidas + 1)) {
                partidas++;
            } else {
                sem_post(&jug[a].listo);
                sem_post(&jug[b].listo);
                sem_post(&mesas_libres);
            }
        }

        if (salir) break;
    }

    // ya no quedan jugadores por emparejar, espero a que terminen las partidas
    // (cada juez hace post en avisos al salir)
    while (mesas_en_uso() > 0)
        esperar(&avisos);

    for (int i = 0; i < M; i++)
        if (juez_creado[i])
            pthread_join(jueces[i], NULL);
    for (int i = 0; i < N; i++)
        pthread_join(jug[i].hilo, NULL);


    for (int i = 0; i < N; i++)
        sem_destroy(&jug[i].listo);
    sem_destroy(&mesas_libres);
    sem_destroy(&avisos);
    pthread_mutex_destroy(&mcola);
    pthread_mutex_destroy(&mmesas);
    pthread_mutex_destroy(&mlog);
    free(jug);
    free(cola);
    free(mesa_ocupada);
    free(juez_creado);
    free(jueces);
    if (flog != NULL) fclose(flog);

    printf("\nfin del torneo, se jugaron %d partidas\n", partidas);
    return 0;
}
