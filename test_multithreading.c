/**
 * test_multithreading.c
 *
 * Simulateur de clients TFTP concurrents.
 * Ce programme lance plusieurs threads, chacun exécutant le binaire
 * "./client" pour simuler des accès simultanés au serveur.
 *
 * Scénarios testés :
 *  1. Lecture simultanée (RRQ) du même fichier par 3 clients.
 *  2. Lecture simultanée de fichiers différents.
 *  3. Conflit écriture (WRQ) vs lecture (RRQ) sur le même fichier.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define IP_SERVEUR "127.0.0.1"

typedef struct {
    int    id_thread;
    char   operation[4];
    char   nom_fichier[256];
} ParametresTest;

void* simuler_client(void* arg) {
    ParametresTest *p = (ParametresTest*)arg;
    char cmd[512];

    /* On redirige la sortie pour ne pas polluer l'affichage, 
       mais les erreurs s'afficheront s'il y en a */
    snprintf(cmd, sizeof(cmd), "../client %s %s %s > /dev/null",
             IP_SERVEUR, p->operation, p->nom_fichier);

    printf("[Thread %d] Départ : %s %s\n", p->id_thread, p->operation, p->nom_fichier);

    int ret = system(cmd);
    if (WIFEXITED(ret) && WEXITSTATUS(ret) == 0)
        printf("[Thread %d] ✓ Succès\n", p->id_thread);
    else
        printf("[Thread %d] ✗ Échec (code %d)\n", p->id_thread, WEXITSTATUS(ret));

    return NULL;
}

int main(void) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║         TESTS DE CONCURRENCE TFTP (Fichiers Multi)       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* Préparation : créer un répertoire d'exécution isolé et y copier
       les sources (comme auparavant le binaire s'exécutait dans
       test/ et copiait ../server.c, ../client.c). */
    system("mkdir -p test_runtime && (cp ../server.c test_runtime/ || cp server.c test_runtime/) && (cp ../client.c test_runtime/ || cp client.c test_runtime/) && (cp ../tftp_protocole.h test_runtime/ || cp tftp_protocole.h test_runtime/)");
    if (chdir("test_runtime") != 0) {
        perror("chdir test_runtime");
        return 1;
    }

    /* SCÉNARIO 1 : Plusieurs PUT simultanés du MÊME fichier */
    printf("━━━ SCÉNARIO 1 : 3 PUT simultanés sur 'server.c' ━━━\n");
    pthread_t t1[3];
    ParametresTest p1[3];
    /* Les sources ont été copiées dans test_runtime ci-dessus. */
    for (int i = 0; i < 3; i++) {
        p1[i].id_thread = i + 1;
        strcpy(p1[i].operation, "put");
        strcpy(p1[i].nom_fichier, "server.c");
        pthread_create(&t1[i], NULL, simuler_client, &p1[i]);
    }
    for (int i = 0; i < 3; i++) pthread_join(t1[i], NULL);
    printf("\n");

    /* SCÉNARIO 2 : LECTURE et PUT en même temps sur le même fichier */
    printf("━━━ SCÉNARIO 2 : GET et PUT simultanés sur 'client.c' ━━━\n");
    pthread_t t2_get, t2_put;
    ParametresTest p2_get = {4, "get", "client.c"};
    ParametresTest p2_put = {5, "put", "client.c"};
    /* le fichier client.c est déjà présent dans test_runtime */

    pthread_create(&t2_put, NULL, simuler_client, &p2_put);
    struct timespec ts = {0, 20000000L}; /* 20ms */
    nanosleep(&ts, NULL);
    pthread_create(&t2_get, NULL, simuler_client, &p2_get);

    pthread_join(t2_put, NULL);
    pthread_join(t2_get, NULL);
    printf("\n");

    /* SCÉNARIO 3 : Plusieurs GET simultanés */
    printf("━━━ SCÉNARIO 3 : 3 GET simultanés sur 'client.c' ━━━\n");
    pthread_t t3[3];
    ParametresTest p3[3];
    for (int i = 0; i < 3; i++) {
        p3[i].id_thread = i + 6;
        strcpy(p3[i].operation, "get");
        strcpy(p3[i].nom_fichier, "client.c");
        pthread_create(&t3[i], NULL, simuler_client, &p3[i]);
    }
    for (int i = 0; i < 3; i++) pthread_join(t3[i], NULL);

    /* Nettoyage : revenir en arrière et supprimer le répertoire temporaire */
    if (chdir("..") != 0) perror("chdir ..");
    system("rm -rf test_runtime");

    printf("\n🏁 [TESTS TERMINÉS]\n");
    return 0;
}