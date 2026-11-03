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
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>

#define FILAS 7
#define COLS 5
#define VACIO -1
#define NPERROS 12
#define NFICHAS (NPERROS + 1)  // ficha 0 = puma, 1..12 = perros
#define MAX_MOVS 16            // como mucho 8 vecinos + 8 saltos
#define JUEZ -1                // "quien" cuando el mutex lo toma el juez o el servidor

#define REVISION_MS 250        // cada cuanto despierta el juez a revisar los latidos
#define MARGEN_LATIDO_MS 1000  // si una ficha no da latido en rmax + esto, se cayo
#define MAX_ARG 100000         // tope para N y M

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
int modo_falla = 2;  // 0 = muere con el mutex tomado, 1 = suelta el mutex y muere, 2 = al azar
int puma_ini[2] = {2, 2};
int perros_ini[NPERROS][2] = {
    {5, 0}, {5, 1}, {5, 2}, {5, 3}, {5, 4},
    {6, 0}, {6, 1}, {6, 2}, {6, 3}, {6, 4},
    {4, 0}, {4, 4}
};

typedef struct partida Partida;

typedef struct {
    int id;  // 0 es el puma, los demas son perros
    int fila, col;
    int vivo;     // 0 si el puma se lo comio
    int salio;    // 1 si el hilo termino normal. si queda en 0 es que se cayo
    long latido;  // ultima vez que tomo el mutex (ms, reloj monotonico)
    unsigned int seed;
    pthread_t hilo;
    Partida *p;
} Ficha;

struct partida {
    int num;
    int mesa;
    int jpuma, jperros;
    int tab[FILAS][COLS];
    Ficha fichas[NFICHAS];
    int capturas;
    int movs;
    int terminado;
    int resultado;
    int abandono;
    int dueno;            // que ficha tiene el mutex ahora (JUEZ = juez o servidor)
    int caida;            // ficha que se cayo (-1 = ninguna), la anota el que lo detecta
    int caida_con_mutex;  // 1 si se detecto por EOWNERDEAD, 0 si por falta de latido
    int caida_avisada;    // el juez ya informo esa caida
    int hubo_falla;       // para simular a lo mas una caida por partida
    pthread_mutex_t mutex;
    sem_t fin;  // aqui duerme el juez: se hace post al terminar o al detectar una caida
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
int max_activas = 0;  // maximo de mesas ocupadas al mismo tiempo (para verificar el limite M)
Partida **en_mesa;    // para poder cancelar las partidas con ctrl+c
int *juez_creado;
pthread_t *jueces;
pthread_mutex_t mmesas = PTHREAD_MUTEX_INITIALIZER;

FILE *flog = NULL;
pthread_mutex_t mlog = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t parar = 0;
sem_t cierre;      // con ctrl+c se despierta a los jugadores que todavia no llegan
int n_handler;     // copia de N para el handler (N puede cambiar si falla un pthread_create)


void error_fatal(const char *que, int err)
{
    fprintf(stderr, "error en %s: %s\n", que, strerror(err));
    exit(1);
}

long ahora_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

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

// bloquea o desbloquea SIGINT en el hilo que la llama. los hilos nuevos heredan
// la mascara, asi que si se crean con SIGINT bloqueada la señal solo la atiende el main
void sigint_bloqueada(int bloquear)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGINT);
    pthread_sigmask(bloquear ? SIG_BLOCK : SIG_UNBLOCK, &s, NULL);
}

void ctrl_c(int s)
{
    (void)s;
    int e = errno;  // sem_post podria cambiar errno y el main lo esta usando
    parar = 1;
    // sem_post se puede usar dentro de un handler (es async-signal-safe), printf no
    sem_post(&avisos);
    sem_post(&mesas_libres);
    for (int i = 0; i < n_handler; i++)
        sem_post(&cierre);
    errno = e;
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

// como esperar() pero con limite de tiempo. devuelve 1 si llego el post, 0 si se acabo el tiempo
int esperar_ms(sem_t *s, int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    while (sem_timedwait(s, &t) != 0) {
        if (errno == ETIMEDOUT) return 0;
        if (errno != EINTR) {
            perror("sem_timedwait");
            exit(1);
        }
    }
    return 1;
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
    int movf[MAX_MOVS], movc[MAX_MOVS], salto[MAX_MOVS];
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

// escribe una linea y el tablero en el log de una sola vez, para que no se
// meta la linea de otra mesa entre el movimiento y su tablero
void log_con_tablero(Partida *p, const char *formato, ...)
{
    if (flog == NULL) return;
    va_list args;
    va_start(args, formato);
    pthread_mutex_lock(&mlog);
    vfprintf(flog, formato, args);
    imprimir(flog, p);
    fflush(flog);
    pthread_mutex_unlock(&mlog);
    va_end(args);
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
// lo pide recibe EOWNERDEAD en vez de quedarse bloqueado para siempre.
// ese hilo solo repara el mutex y le avisa al juez, el abandono lo declara el juez
void tomar(Partida *p, int quien)
{
    int r = pthread_mutex_lock(&p->mutex);
    if (r == EOWNERDEAD) {
        r = pthread_mutex_consistent(&p->mutex);
        if (r != 0) error_fatal("pthread_mutex_consistent", r);
        escribir_log("[mesa %d] la ficha %d murio con el mutex tomado\n", p->mesa + 1, p->dueno);
        if (p->caida < 0) {
            p->caida = p->dueno;  // el que murio quedo anotado como dueno
            p->caida_con_mutex = 1;
        }
        sem_post(&p->fin);  // despierto al juez
    } else if (r != 0) {
        error_fatal("pthread_mutex_lock", r);
    }
    p->dueno = quien;
    if (quien != JUEZ) p->fichas[quien].latido = ahora_ms();
}

// la llama el juez con el mutex tomado
void declarar_abandono(Partida *p)
{
    int i = p->caida;
    p->caida_avisada = 1;
    printf("[mesa %d] el juez detecto que la ficha %d (%s) se cayo: %s\n", p->mesa + 1, i,
           i == 0 ? "puma" : "perro", p->caida_con_mutex ? "murio con el mutex tomado" : "dejo de dar latido");
    escribir_log("[mesa %d] el juez detecto que la ficha %d se cayo\n", p->mesa + 1, i);
    if (!p->terminado) {
        p->abandono = 1;
        terminar(p, i == 0 ? GANAN_PERROS : GANA_PUMA);
    }
}

// la llama el juez con el mutex tomado. una ficha viva toma el mutex al menos
// cada rmax ms, si lleva mucho mas sin hacerlo es que el hilo se murio
void revisar_latidos(Partida *p, int creadas)
{
    long ahora = ahora_ms();
    for (int i = 0; i < creadas; i++) {
        Ficha *fi = &p->fichas[i];
        if (fi->vivo && !fi->salio && ahora - fi->latido > rmax + MARGEN_LATIDO_MS) {
            p->caida = i;
            p->caida_con_mutex = 0;
            return;
        }
    }
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
    if (comido > 0) {
        p->tab[mf][mc] = VACIO;
        p->fichas[comido].vivo = 0;
        p->capturas++;
        printf("[mesa %d] el puma se comio al perro %d (lleva %d)\n", p->mesa + 1, comido, p->capturas);
    }
    p->movs++;

    char extra[48] = "";
    if (comido > 0) snprintf(extra, sizeof(extra), " y se come al perro %d", comido);
    log_con_tablero(p, "[mesa %d] ficha %d: (%d,%d) -> (%d,%d)%s\n", p->mesa + 1, fi->id, of, oc, df, dc, extra);

    // ver si alguien gano
    int movf[MAX_MOVS], movc[MAX_MOVS], salto[MAX_MOVS];
    Ficha *puma = &p->fichas[0];
    if (p->capturas >= 6)
        terminar(p, GANA_PUMA);
    else if (movimientos(p->tab, 0, puma->fila, puma->col, movf, movc, salto) == 0)
        terminar(p, GANAN_PERROS);
    else if (p->movs >= max_movs)
        terminar(p, EMPATE);
}

// con el mutex tomado: si la ficha ya no tiene que jugar, anota que salio
// normal, suelta el mutex y devuelve 1
int salir_si_corresponde(Partida *p, Ficha *fi)
{
    if (!p->terminado && p->caida < 0 && fi->vivo) return 0;
    fi->salio = 1;
    pthread_mutex_unlock(&p->mutex);
    return 1;
}

// para probar las desconexiones: el hilo se muere de golpe sin avisarle a nadie.
// segun modo_falla se muere con el mutex tomado o despues de soltarlo
_Noreturn void simular_caida(Partida *p, Ficha *fi)
{
    p->hubo_falla = 1;
    int con_mutex = modo_falla == 0 || (modo_falla == 2 && rand_r(&fi->seed) % 2 == 0);
    printf("[mesa %d] (prueba) se cae la ficha %d %s\n", p->mesa + 1, fi->id,
           con_mutex ? "con el mutex tomado" : "sin el mutex");
    if (!con_mutex) pthread_mutex_unlock(&p->mutex);
    pthread_exit(NULL);
}

void *ficha(void *arg)
{
    Ficha *fi = arg;
    Partida *p = fi->p;
    int of, oc, df = 0, dc = 0;

    while (1) {
        tomar(p, fi->id);
        if (salir_si_corresponde(p, fi)) break;
        of = fi->fila;
        oc = fi->col;
        int puedo = elegir(p->tab, fi, &df, &dc);
        pthread_mutex_unlock(&p->mutex);

        // "pensando"... mientras tanto los demas pueden moverse
        dormir(rmin + rand_r(&fi->seed) % (rmax - rmin + 1));

        if (!puedo) continue;

        tomar(p, fi->id);
        if (salir_si_corresponde(p, fi)) break;

        // a lo mas una caida simulada por partida, asi la prueba puede comparar
        // caidas simuladas con caidas detectadas
        if (prob_falla > 0 && !p->hubo_falla && (int)(rand_r(&fi->seed) % 1000) < prob_falla)
            simular_caida(p, fi);

        mover(p, fi, of, oc, df, dc);
        pthread_mutex_unlock(&p->mutex);
    }
    return NULL;
}

// lo ultimo que hace el juez: avisa el resultado a los jugadores y deja la mesa libre
void liberar_mesa(Partida *p)
{
    jug[p->jpuma].resultado = p->resultado;
    jug[p->jpuma].abandono = p->abandono;
    jug[p->jperros].resultado = p->resultado;
    jug[p->jperros].abandono = p->abandono;
    sem_post(&jug[p->jpuma].listo);
    sem_post(&jug[p->jperros].listo);

    int m = p->mesa;
    pthread_mutex_lock(&mmesas);
    en_mesa[m] = NULL;
    mesa_ocupada[m] = 0;
    pthread_mutex_unlock(&mmesas);
    free(p);

    sem_post(&mesas_libres);
    sem_post(&avisos);  // le aviso al servidor que se desocupo una mesa
}

// mutex robusto + semaforo de la partida. devuelve 0 si algo fallo (y no deja nada creado)
int iniciar_primitivas(Partida *p)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) return 0;
    int ok = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 &&
             pthread_mutex_init(&p->mutex, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!ok) return 0;
    if (sem_init(&p->fin, 0, 0) != 0) {
        pthread_mutex_destroy(&p->mutex);
        return 0;
    }
    return 1;
}

void *juez(void *arg)
{
    Partida *p = arg;
    const char *txt[] = {"sin jugar", "gana el puma", "ganan los perros", "empate", "cancelada"};

    // 1. primitivas y tablero
    if (!iniciar_primitivas(p)) {
        fprintf(stderr, "[mesa %d] no se pudo crear el mutex o el semaforo, se cancela la partida %d\n", p->mesa + 1, p->num);
        p->resultado = CANCELADA;
        p->abandono = 0;
        liberar_mesa(p);
        return NULL;
    }

    for (int i = 0; i < FILAS; i++)
        for (int j = 0; j < COLS; j++)
            p->tab[i][j] = VACIO;

    long t0 = ahora_ms();
    for (int i = 0; i < NFICHAS; i++) {
        Ficha *fi = &p->fichas[i];
        fi->id = i;
        fi->vivo = 1;
        fi->salio = 0;
        fi->latido = t0;
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
    p->dueno = JUEZ;
    p->caida = -1;
    p->caida_con_mutex = 0;
    p->caida_avisada = 0;
    p->hubo_falla = 0;

    // recien ahora el servidor puede ver la partida (para cancelarla con ctrl+c)
    pthread_mutex_lock(&mmesas);
    en_mesa[p->mesa] = p;
    pthread_mutex_unlock(&mmesas);

    // flockfile: mientras imprimo el tablero ningun otro hilo puede escribir en
    // la consola, asi el tablero no queda cortado por la linea de otro hilo
    flockfile(stdout);
    printf("[mesa %d] empieza la partida %d: jugador %d (puma) vs jugador %d (perros)\n", p->mesa + 1, p->num, p->jpuma, p->jperros);
    imprimir(stdout, p);
    funlockfile(stdout);
    log_con_tablero(p, "[mesa %d] ======= empieza partida %d =======\n", p->mesa + 1, p->num);

    // por si la disposicion inicial de la config ya deja al puma encerrado
    // (con el mutex, porque el servidor ya podria estar cancelando la partida)
    int movf[MAX_MOVS], movc[MAX_MOVS], salto[MAX_MOVS];
    tomar(p, JUEZ);
    if (movimientos(p->tab, 0, puma_ini[0], puma_ini[1], movf, movc, salto) == 0)
        terminar(p, GANAN_PERROS);
    pthread_mutex_unlock(&p->mutex);

    // 2. lanzar las fichas
    int creadas = 0;
    for (int i = 0; i < NFICHAS; i++) {
        if (pthread_create(&p->fichas[i].hilo, NULL, ficha, &p->fichas[i]) != 0) {
            fprintf(stderr, "[mesa %d] error creando el hilo de la ficha %d, se cancela la partida\n", p->mesa + 1, i);
            tomar(p, JUEZ);
            terminar(p, CANCELADA);
            pthread_mutex_unlock(&p->mutex);
            break;
        }
        creadas++;
    }

    // si el ctrl+c llego justo mientras se armaba la partida
    if (parar) {
        tomar(p, JUEZ);
        terminar(p, CANCELADA);
        pthread_mutex_unlock(&p->mutex);
    }

    // 3. vigilar la partida. el juez duerme en p->fin y despierta cuando la partida
    // termina, cuando alguien le avisa de una caida, o cada REVISION_MS para
    // revisar los latidos (eso cubre a una ficha que se muere sin tener el mutex)
    while (1) {
        esperar_ms(&p->fin, REVISION_MS);
        tomar(p, JUEZ);  // si una ficha murio con el mutex, aqui tambien se detecta
        if (!p->terminado && p->caida < 0)
            revisar_latidos(p, creadas);
        if (p->caida >= 0 && !p->caida_avisada)
            declarar_abandono(p);
        int listo = p->terminado;
        pthread_mutex_unlock(&p->mutex);
        if (listo) break;
    }

    for (int i = 0; i < creadas; i++)
        pthread_join(p->fichas[i].hilo, NULL);

    // una ficha que termino sin marcar "salio" se cayo. si fue justo cuando la partida
    // ya se estaba cerrando (antes de que el juez la viera), igual queda registrado
    for (int i = 0; i < creadas; i++) {
        if (!p->fichas[i].salio && !(p->caida_avisada && p->caida == i)) {
            printf("[mesa %d] el juez detecto que la ficha %d se cayo (al cerrar la partida, el resultado no cambia)\n", p->mesa + 1, i);
            escribir_log("[mesa %d] la ficha %d se cayo al final de la partida\n", p->mesa + 1, i);
        }
    }

    flockfile(stdout);
    printf("[mesa %d] termino la partida %d: %s%s (%d movimientos, %d capturas)\n", p->mesa + 1, p->num,
           txt[p->resultado], p->abandono ? " por abandono" : "", p->movs, p->capturas);
    imprimir(stdout, p);
    funlockfile(stdout);
    escribir_log("[mesa %d] ======= fin partida %d: %s%s =======\n", p->mesa + 1, p->num,
                 txt[p->resultado], p->abandono ? " por abandono" : "");

    // 4. destruir las primitivas de la partida, avisar a los jugadores y liberar la mesa
    int r = pthread_mutex_destroy(&p->mutex);
    if (r != 0) fprintf(stderr, "[mesa %d] pthread_mutex_destroy: %s\n", p->mesa + 1, strerror(r));
    if (sem_destroy(&p->fin) != 0) perror("sem_destroy");
    liberar_mesa(p);
    return NULL;
}

void *jugador(void *arg)
{
    Jugador *j = arg;
    unsigned int s = semilla + 999 + j->id;

    // los jugadores llegan en distintos momentos (0 a 2 s). en vez de dormir
    // espero en "cierre" con timeout: si hay ctrl+c llego altiro y el servidor
    // me rechaza, asi el cierre no tiene que esperar a que despierte
    esperar_ms(&cierre, rand_r(&s) % 2000);

    // siempre me anoto en la cola, si el torneo esta cerrado el servidor me rechaza
    pthread_mutex_lock(&mcola);
    cola[fin] = j->id;
    fin++;
    llegaron++;
    if (!parar) printf("[jugador %d] llego, esperando rival\n", j->id);
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

// la llama el servidor despues del ctrl+c
void cancelar_partidas(void)
{
    pthread_mutex_lock(&mmesas);
    for (int i = 0; i < M; i++) {
        if (en_mesa[i] != NULL) {
            tomar(en_mesa[i], JUEZ);
            terminar(en_mesa[i], CANCELADA);
            pthread_mutex_unlock(&en_mesa[i]->mutex);
        }
    }
    pthread_mutex_unlock(&mmesas);
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
        fprintf(stderr, "[servidor] no hay memoria para otra partida\n");
        return 0;
    }

    int m = 0, activas = 0;
    pthread_mutex_lock(&mmesas);
    while (m < M && mesa_ocupada[m]) m++;
    if (m == M) {
        // no deberia pasar nunca: mesas_libres dijo que habia una libre
        pthread_mutex_unlock(&mmesas);
        fprintf(stderr, "[servidor] error: no hay mesa libre\n");
        free(p);
        return 0;
    }
    mesa_ocupada[m] = 1;
    for (int i = 0; i < M; i++)
        activas += mesa_ocupada[i];
    if (activas > max_activas) max_activas = activas;
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

    // el juez (y sus fichas) nacen con SIGINT bloqueada, la atiende solo el main
    sigint_bloqueada(1);
    int r = pthread_create(&jueces[m], NULL, juez, p);
    sigint_bloqueada(0);
    if (r != 0) {
        fprintf(stderr, "[servidor] no se pudo crear el juez de la mesa %d\n", m + 1);
        free(p);
        pthread_mutex_lock(&mmesas);
        mesa_ocupada[m] = 0;
        pthread_mutex_unlock(&mmesas);
        return 0;
    }
    juez_creado[m] = 1;
    return 1;
}

// devuelve 0 si el archivo no se pudo leer o tiene errores
int leer_config(const char *archivo)
{
    FILE *f = fopen(archivo, "r");
    if (f == NULL) {
        fprintf(stderr, "error: no se pudo abrir %s: %s\n", archivo, strerror(errno));
        return 0;
    }
    char linea[200], nombre[50];
    int a, b, nlinea = 0, perros_leidos = 0, ok = 1;

    while (fgets(linea, sizeof(linea), f)) {
        nlinea++;
        int n = sscanf(linea, " %49s %d %d", nombre, &a, &b);
        if (n <= 0 || nombre[0] == '#') continue;  // linea vacia o comentario
        if (n < 2) {
            fprintf(stderr, "config linea %d: falta el valor de '%s'\n", nlinea, nombre);
            ok = 0;
            continue;
        }

        if (strcmp(nombre, "retardo_min") == 0) rmin = a;
        else if (strcmp(nombre, "retardo_max") == 0) rmax = a;
        else if (strcmp(nombre, "semilla") == 0) semilla = (unsigned int)a;
        else if (strcmp(nombre, "max_movimientos") == 0) max_movs = a;
        else if (strcmp(nombre, "prob_falla") == 0) prob_falla = a;
        else if (strcmp(nombre, "modo_falla") == 0) modo_falla = a;
        else if (strcmp(nombre, "puma") == 0 || strcmp(nombre, "perro") == 0) {
            if (n < 3) {
                fprintf(stderr, "config linea %d: '%s' necesita fila y columna\n", nlinea, nombre);
                ok = 0;
            } else if (nombre[1] == 'u') {
                puma_ini[0] = a;
                puma_ini[1] = b;
            } else if (perros_leidos == NPERROS) {
                fprintf(stderr, "config linea %d: hay mas de %d perros\n", nlinea, NPERROS);
                ok = 0;
            } else {
                perros_ini[perros_leidos][0] = a;
                perros_ini[perros_leidos][1] = b;
                perros_leidos++;
            }
        } else {
            // no es error, pero aviso por si hay un nombre mal escrito
            fprintf(stderr, "aviso: config linea %d: no conozco '%s', la ignoro\n", nlinea, nombre);
        }
    }
    fclose(f);

    // o se dan los 12 perros o ninguno (y se usa la posicion por defecto)
    if (perros_leidos != 0 && perros_leidos != NPERROS) {
        fprintf(stderr, "error: la config tiene %d perros y tienen que ser %d\n", perros_leidos, NPERROS);
        ok = 0;
    }
    return ok;
}

int config_ok(void)
{
    if (rmin < 0 || rmax < rmin || rmax > 60000 || max_movs <= 0) {
        fprintf(stderr, "error: revisar retardos (0 <= min <= max <= 60000) o max_movimientos (> 0) en la config\n");
        return 0;
    }
    if (prob_falla < 0 || prob_falla > 1000 || modo_falla < 0 || modo_falla > 2) {
        fprintf(stderr, "error: prob_falla va de 0 a 1000 y modo_falla de 0 a 2\n");
        return 0;
    }
    int usado[FILAS][COLS] = {{0}};
    if (!existe(puma_ini[0], puma_ini[1])) {
        fprintf(stderr, "error: el puma esta fuera del tablero (%d,%d)\n", puma_ini[0], puma_ini[1]);
        return 0;
    }
    usado[puma_ini[0]][puma_ini[1]] = 1;
    for (int i = 0; i < NPERROS; i++) {
        int f = perros_ini[i][0], c = perros_ini[i][1];
        if (!existe(f, c) || usado[f][c]) {
            fprintf(stderr, "error: el perro %d esta mal puesto (%d,%d)\n", i + 1, f, c);
            return 0;
        }
        usado[f][c] = 1;
    }
    return 1;
}

// lee un entero positivo completo ("5x" o "abc" no sirven). devuelve 0 si no es valido
int leer_entero(const char *s, int *out)
{
    char *resto;
    errno = 0;
    long v = strtol(s, &resto, 10);
    if (errno != 0 || resto == s || *resto != '\0' || v <= 0 || v > MAX_ARG) return 0;
    *out = (int)v;
    return 1;
}

void liberar_arreglos(void)
{
    free(jug);
    free(cola);
    free(mesa_ocupada);
    free(en_mesa);
    free(juez_creado);
    free(jueces);
}

int main(int argc, char *argv[])
{
    // salida por linea: si stdout va a un archivo o pipe y el programa muere de
    // golpe, no se pierde lo que ya se imprimio
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "uso: %s N M [config.txt]\n", argv[0]);
        return 1;
    }
    if (!leer_entero(argv[1], &N) || !leer_entero(argv[2], &M)) {
        fprintf(stderr, "error: N y M tienen que ser enteros entre 1 y %d\n", MAX_ARG);
        return 1;
    }

    semilla = time(NULL);
    if (argc == 4 && !leer_config(argv[3])) return 1;
    if (!config_ok()) return 1;

    jug = malloc(N * sizeof(Jugador));
    cola = malloc(N * sizeof(int));
    mesa_ocupada = calloc(M, sizeof(int));
    en_mesa = calloc(M, sizeof(Partida *));
    juez_creado = calloc(M, sizeof(int));
    jueces = malloc(M * sizeof(pthread_t));
    if (!jug || !cola || !mesa_ocupada || !en_mesa || !juez_creado || !jueces) {
        fprintf(stderr, "error: no hay memoria\n");
        liberar_arreglos();
        return 1;
    }

    if (sem_init(&mesas_libres, 0, M) != 0) {
        perror("sem_init");
        liberar_arreglos();
        return 1;
    }
    if (sem_init(&avisos, 0, 0) != 0) {
        perror("sem_init");
        sem_destroy(&mesas_libres);
        liberar_arreglos();
        return 1;
    }
    if (sem_init(&cierre, 0, 0) != 0) {
        perror("sem_init");
        sem_destroy(&mesas_libres);
        sem_destroy(&avisos);
        liberar_arreglos();
        return 1;
    }

    flog = fopen("torneo.log", "w");
    if (flog == NULL) fprintf(stderr, "aviso: no se pudo crear torneo.log, sigo sin log\n");

    printf("=== Torneo de Komikan ===\n");
    printf("%d jugadores, %d mesas, retardo %d-%d ms, semilla %u\n\n", N, M, rmin, rmax, semilla);

    n_handler = N;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ctrl_c;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;  // sem_wait igual vuelve con EINTR, por eso esta esperar()
    if (sigaction(SIGINT, &sa, NULL) != 0) perror("sigaction");

    // los jugadores nacen con SIGINT bloqueada: el handler siempre corre en el main
    sigint_bloqueada(1);
    for (int i = 0; i < N; i++) {
        jug[i].id = i;
        jug[i].rol = -1;
        jug[i].mesa = -1;
        jug[i].resultado = SIN_JUGAR;
        jug[i].abandono = 0;
        if (sem_init(&jug[i].listo, 0, 0) != 0) {
            perror("sem_init");
            fprintf(stderr, "el torneo sigue con %d jugadores\n", i);
            N = i;
            break;
        }
        if (pthread_create(&jug[i].hilo, NULL, jugador, &jug[i]) != 0) {
            // si no se pudo crear, sigo con los que ya estan
            fprintf(stderr, "error creando el jugador %d, el torneo sigue con %d jugadores\n", i, i);
            sem_destroy(&jug[i].listo);
            N = i;
            break;
        }
    }
    sigint_bloqueada(0);

    int partidas = 0, cancelado = 0;
    while (N > 0) {
        esperar(&avisos);  // dormido hasta que llegue alguien o haya ctrl+c

        if (parar && !cancelado) {
            printf("\n[servidor] se apreto ctrl+c, no se aceptan mas jugadores\n");
            cancelar_partidas();
            cancelado = 1;
        }

        int a = -1, b = -1, salir = 0;
        pthread_mutex_lock(&mcola);
        if (parar) {
            // rechazo a todos los que estan esperando o que van llegando
            while (ini < fin) {
                printf("[servidor] jugador %d rechazado, el torneo esta cerrado\n", cola[ini]);
                sem_post(&jug[cola[ini]].listo);
                ini++;
            }
        } else if (fin - ini >= 2) {
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
            if (parar) {
                sem_post(&jug[a].listo);
                sem_post(&jug[b].listo);
            } else if (nueva_partida(a, b, partidas + 1)) {
                partidas++;
            } else {
                sem_post(&jug[a].listo);
                sem_post(&jug[b].listo);
                sem_post(&mesas_libres);
            }
        }

        if (salir) break;
    }

    // ya no quedan jugadores por emparejar, pero sigo atento al ctrl+c
    // hasta que terminen todas las partidas (cada juez hace post en avisos al salir)
    while (mesas_en_uso() > 0) {
        esperar(&avisos);
        if (parar && !cancelado) {
            printf("\n[servidor] se apreto ctrl+c, cancelando las partidas que quedan\n");
            cancelar_partidas();
            cancelado = 1;
        }
    }

    for (int i = 0; i < M; i++)
        if (juez_creado[i])
            pthread_join(jueces[i], NULL);
    for (int i = 0; i < N; i++)
        pthread_join(jug[i].hilo, NULL);

    // desde aqui se ignora el ctrl+c: el handler usa los semaforos que voy a destruir
    struct sigaction ign;
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, NULL);

    for (int i = 0; i < N; i++)
        sem_destroy(&jug[i].listo);
    sem_destroy(&mesas_libres);
    sem_destroy(&avisos);
    sem_destroy(&cierre);
    pthread_mutex_destroy(&mcola);
    pthread_mutex_destroy(&mmesas);
    pthread_mutex_destroy(&mlog);
    liberar_arreglos();
    if (flog != NULL && fclose(flog) != 0) perror("fclose torneo.log");

    printf("\nmaximo de mesas jugando a la vez: %d (limite M = %d)\n", max_activas, M);
    printf("fin del torneo, se jugaron %d partidas%s\n", partidas, parar ? " (cerrado con ctrl+c)" : "");
    return 0;
}
