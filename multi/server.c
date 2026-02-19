#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <dirent.h>
#include "tftp_protocole.h"
#include "tftp_errors.h"

#define TFTP_ROOT "./.tftp"  // Répertoire racine des fichiers TFTP

/**
 * MECANISME DE SYNCHRONISATION LECTEUR-ECRIVAIN
 *
 * Règles :
 *  - Plusieurs lecteurs peuvent lire SIMULTANEMENT (pas de blocage entre eux).
 *  - Un lecteur attend seulement si un ECRIVAIN est en cours.
 *  - Un écrivain attend que TOUS les lecteurs soient partis ET qu'aucun
 *    autre écrivain n'est actif, puis prend le fichier seul.
 *
 * Implémentation :
 *   mutex + variable de condition + compteur de lecteurs + flag écrivain.
 *   (Plus explicite qu'un pthread_rwlock)
 */
typedef struct VerrouFichier {
    char nom_fichier[256];

    pthread_mutex_t mutex;         /* Protège nb_lecteurs et en_ecriture */
    pthread_cond_t  cond;          /* Réveille les threads en attente    */
    int nb_lecteurs;               /* Nombre de lecteurs actifs          */
    int en_ecriture;               /* 1 si un écrivain est actif         */

    int ref_count;                 /* Nb de threads utilisant ce verrou  */
    struct VerrouFichier *suivant;
} VerrouFichier;

VerrouFichier *liste_verrous = NULL;
pthread_mutex_t mutex_liste = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------
 * Recherche ou crée un verrou pour un fichier donné (opération atomique).
 * ------------------------------------------------------------------ */
VerrouFichier* obtenir_verrou(const char *nom) {
    pthread_mutex_lock(&mutex_liste);
    VerrouFichier *courant = liste_verrous;

    /* 1. Chercher un verrou existant */
    while (courant) {
        if (strcmp(courant->nom_fichier, nom) == 0) {
            courant->ref_count++;
            pthread_mutex_unlock(&mutex_liste);
            return courant;
        }
        courant = courant->suivant;
    }

    /* 2. Créer un nouveau verrou */
    VerrouFichier *nouveau = malloc(sizeof(VerrouFichier));
    if (!nouveau) { pthread_mutex_unlock(&mutex_liste); return NULL; }

    memset(nouveau, 0, sizeof(VerrouFichier));
    strncpy(nouveau->nom_fichier, nom, sizeof(nouveau->nom_fichier) - 1);
    pthread_mutex_init(&nouveau->mutex, NULL);
    pthread_cond_init(&nouveau->cond, NULL);
    nouveau->nb_lecteurs  = 0;
    nouveau->en_ecriture  = 0;
    nouveau->ref_count    = 1;
    nouveau->suivant      = liste_verrous;
    liste_verrous         = nouveau;

    pthread_mutex_unlock(&mutex_liste);
    return nouveau;
}

void relacher_verrou(VerrouFichier *v) {
    pthread_mutex_lock(&mutex_liste);
    v->ref_count--;
    
    /* Si plus personne n'utilise ce fichier, on nettoie la mémoire */
    if (v->ref_count == 0) {
        VerrouFichier **pp = &liste_verrous;
        while (*pp && *pp != v) pp = &((*pp)->suivant);
        if (*pp) *pp = v->suivant; /* Retire de la liste chaînée */

        pthread_mutex_destroy(&v->mutex);
        pthread_cond_destroy(&v->cond);
        free(v);
    }
    pthread_mutex_unlock(&mutex_liste);
}

// --- FONCTIONS UTILITAIRES D'ERREUR ---

void envoyer_erreur(tftp_context_t *ctx, uint16_t err_code, const char *msg) {
    tftp_error_packet_t pkt;
    pkt.opcode = htons(TFTP_OP_ERR); //
    pkt.err_code = htons(err_code); //
    
    if (msg) {
        strncpy(pkt.message, msg, sizeof(pkt.message) - 1);
        pkt.message[sizeof(pkt.message) - 1] = '\0';
    } else if (err_code < 8) {
        strcpy(pkt.message, tftp_error_messages[err_code]); //
    }
    
    size_t pkt_len = 4 + strlen(pkt.message) + 1;
    sendto(ctx->sockfd, &pkt, pkt_len, 0, 
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_len); //
}

void envoyer_ack(tftp_context_t *ctx, uint16_t block) {
    tftp_ack_packet_t ack = {
        .opcode = htons(TFTP_OP_ACK), //
        .block = htons(block) //
    };
    sendto(ctx->sockfd, &ack, sizeof(ack), 0, 
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_len); //
}

int envoyer_donnees(tftp_context_t *ctx, uint16_t block, const uint8_t *data, size_t len) {
    tftp_data_packet_t pkt;
    pkt.opcode = htons(TFTP_OP_DATA); //
    pkt.block = htons(block); //
    memcpy(pkt.data, data, len); //
    
    ssize_t sent = sendto(ctx->sockfd, &pkt, 4 + len, 0,
                         (struct sockaddr *)&ctx->peer_addr, ctx->peer_len); //
    return (sent == (ssize_t)(4 + len)) ? 0 : -1;
}

int verifier_tid(tftp_context_t *ctx, struct sockaddr_in *from) {
    if (ctx->last_block == 0 && ctx->peer_addr.sin_port == 0) {
        ctx->peer_addr.sin_port = from->sin_port; //
        return 1;
    }
    
    if (from->sin_addr.s_addr != ctx->peer_addr.sin_addr.s_addr || 
        from->sin_port != ctx->peer_addr.sin_port) { //
        return 0;
    }
    return 1;
}

char* construire_chemin_securise(char *buffer, size_t sz, const char *filename) {
    if (strstr(filename, "..") || filename[0] == '/') { //
        return NULL;
    }
    snprintf(buffer, sz, "%s/%s", TFTP_ROOT, filename); //
    return buffer;
}

// --- OPÉRATIONS PRINCIPALES (RRQ / WRQ) ---

int executer_rrq(tftp_context_t *ctx) {
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    char chemin[512];
    if (!construire_chemin_securise(chemin, sizeof(chemin), ctx->filename)) {
        printf("[T-%04lu] [ERREUR] Chemin invalide : '%s'\n", tid, ctx->filename);
        envoyer_erreur(ctx, ERR_ACCESS_VIOLATION, "Chemin invalide");
        return -1;
    }

    ctx->fp = fopen(chemin, "rb");
    if (!ctx->fp) {
        printf("[T-%04lu] [ERREUR] Fichier introuvable : '%s' (%s)\n",
               tid, ctx->filename, strerror(errno));
        envoyer_erreur(ctx, ERR_FILE_NOT_FOUND, strerror(errno));
        return -1;
    }
    printf("[T-%04lu] RRQ '%s' → envoi en cours...\n", tid, ctx->filename);
    fflush(stdout);

    uint16_t block = 1;
    uint8_t buf[MAX_DATA_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int done = 0;

    while (!done) {
        size_t n = fread(buf, 1, MAX_DATA_SIZE, ctx->fp);
        ctx->retries = 0;
        int acked = 0;

        while (!acked && ctx->retries < TFTP_MAX_RETRIES) {
            envoyer_donnees(ctx, block, buf, n);
            ssize_t r = recvfrom(ctx->sockfd, buf, MAX_PACKET_SIZE, 0,
                               (struct sockaddr *)&from, &from_len);

            if (r >= 4 && verifier_tid(ctx, &from)) {
                uint16_t op = ntohs(*(uint16_t *)buf);
                uint16_t blk = ntohs(*(uint16_t *)(buf + 2));
                if (op == TFTP_OP_ACK && blk == block) {
                    acked = 1;
                    ctx->last_block = block;
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                printf("[T-%04lu] [TIMEOUT %d/%d] RRQ '%s' bloc %u\n",
                       tid, ++ctx->retries, TFTP_MAX_RETRIES,
                       ctx->filename, block);
                fflush(stdout);
            }
        }
        if (!acked) {
            printf("[T-%04lu] [ECHEC] RRQ '%s' - max retries atteint\n", tid, ctx->filename);
            fclose(ctx->fp); return -1;
        }
        if (n < MAX_DATA_SIZE) done = 1; else block++;
    }
    fclose(ctx->fp);
    printf("[T-%04lu] RRQ '%s' → %u bloc(s) envoyé(s) ✓\n", tid, ctx->filename, block);
    fflush(stdout);
    return 0;
}

int executer_wrq(tftp_context_t *ctx) {
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    char chemin[512];
    if (!construire_chemin_securise(chemin, sizeof(chemin), ctx->filename)) {
        printf("[T-%04lu] [ERREUR] Chemin invalide : '%s'\n", tid, ctx->filename);
        envoyer_erreur(ctx, ERR_ACCESS_VIOLATION, "Chemin invalide");
        return -1;
    }

    /* Ouverture en écriture avec écrasement */
    ctx->fp = fopen(chemin, "wb");
    if (!ctx->fp) {
        printf("[T-%04lu] [ERREUR] Impossible d'ouvrir '%s' en écriture : %s\n",
               tid, ctx->filename, strerror(errno));
        envoyer_erreur(ctx, ERR_ACCESS_VIOLATION, strerror(errno));
        return -1;
    }
    printf("[T-%04lu] WRQ '%s' → réception en cours...\n", tid, ctx->filename);
    fflush(stdout);

    envoyer_ack(ctx, 0); /* ACK du WRQ */
    ctx->last_block = 0;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    uint8_t buf[MAX_PACKET_SIZE];
    uint16_t expected_block = 1;
    int done = 0;

    while (!done) {
        ssize_t n = recvfrom(ctx->sockfd, buf, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 4) continue;
        if (!verifier_tid(ctx, &from)) continue;

        uint16_t op    = ntohs(*(uint16_t *)buf);
        uint16_t block = ntohs(*(uint16_t *)(buf + 2));

        if (op == TFTP_OP_DATA && block == expected_block) {
            size_t written = fwrite(buf + 4, 1, n - 4, ctx->fp);
            if (written != (size_t)(n - 4)) {
                printf("[T-%04lu] [ERREUR] Disque plein lors de l'écriture de '%s'\n",
                       tid, ctx->filename);
                envoyer_erreur(ctx, ERR_DISK_FULL, "Erreur disque");
                fclose(ctx->fp); unlink(chemin); return -1;
            }
            envoyer_ack(ctx, block);
            ctx->last_block = block;
            if (n - 4 < MAX_DATA_SIZE) done = 1; else expected_block++;
        }
    }
    fclose(ctx->fp);
    printf("[T-%04lu] WRQ '%s' → %u bloc(s) reçu(s) ✓\n",
           tid, ctx->filename, ctx->last_block);
    fflush(stdout);
    return 0;
}

// --- LOGIQUE DES THREADS OUVRIERS ---

void* traitement_client(void* arg) {
    tftp_context_t *ctx = (tftp_context_t*)arg;
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    pthread_detach(pthread_self());

    /* 1. Socket dédiée à ce transfert (nouveau TID) */
    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd < 0) {
        printf("[T-%04lu] [ERREUR] Impossible de créer la socket TID\n", tid);
        free(ctx); return NULL;
    }

    struct sockaddr_in serv_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0
    };
    if (bind(ctx->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[T-%04lu] [ERREUR] Bind TID échoué\n", tid);
        close(ctx->sockfd); free(ctx); return NULL;
    }

    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 2. Obtenir le verrou associé au fichier */
    VerrouFichier *v = obtenir_verrou(ctx->filename);
    if (!v) {
        printf("[T-%04lu] [ERREUR] Mémoire insuffisante (verrou)\n", tid);
        close(ctx->sockfd); free(ctx); return NULL;
    }

    if (ctx->est_ecrivain) {
        /* ============================================================
         * MODE ECRITURE (WRQ)
         * Attendre : (a) qu'il n'y ait plus aucun lecteur actif
         *            (b) qu'aucun autre écrivain ne soit en cours
         * ============================================================ */
        pthread_mutex_lock(&v->mutex);
        if (v->nb_lecteurs > 0 || v->en_ecriture) {
            printf("[T-%04lu] ECRITURE '%s' : en attente "
                   "(lecteurs=%d, ecrivain=%s)...\n",
                   tid, ctx->filename,
                   v->nb_lecteurs, v->en_ecriture ? "oui" : "non");
            fflush(stdout);
        }
        while (v->nb_lecteurs > 0 || v->en_ecriture)
            pthread_cond_wait(&v->cond, &v->mutex);
        
        v->en_ecriture = 1;
        printf("[T-%04lu] ECRITURE '%s' : verrou acquis.\n", tid, ctx->filename);
        pthread_mutex_unlock(&v->mutex);

        executer_wrq(ctx);

        pthread_mutex_lock(&v->mutex);
        v->en_ecriture = 0;
        printf("[T-%04lu] ECRITURE '%s' : terminée, libération.\n", tid, ctx->filename);
        pthread_cond_broadcast(&v->cond);
        pthread_mutex_unlock(&v->mutex);
    } else {
        /* ============================================================
         * MODE LECTURE (RRQ)
         * Attendre UNIQUEMENT si un écrivain est actif.
         * Plusieurs lecteurs peuvent lire EN MEME TEMPS sans se bloquer.
         * ============================================================ */
        pthread_mutex_lock(&v->mutex);
        if (v->en_ecriture) {
            printf("[T-%04lu] LECTURE '%s' : attente (écriture en cours)...\n", tid, ctx->filename);
            fflush(stdout);
        }
        while (v->en_ecriture)
            pthread_cond_wait(&v->cond, &v->mutex);

        v->nb_lecteurs++;
        printf("[T-%04lu] LECTURE '%s' : début (%d lecteur(s) actif(s))\n",
               tid, ctx->filename, v->nb_lecteurs);
        pthread_mutex_unlock(&v->mutex);

        executer_rrq(ctx);

        pthread_mutex_lock(&v->mutex);
        v->nb_lecteurs--;
        printf("[T-%04lu] LECTURE '%s' : terminée (%d restant).\n", tid, ctx->filename, v->nb_lecteurs);
        if (v->nb_lecteurs == 0) pthread_cond_broadcast(&v->cond);
        pthread_mutex_unlock(&v->mutex);
    }

    /* 3. Nettoyage final GARANTI */
    printf("[T-%04lu] Session finie : fermeture port TID %d.\n", tid, ntohs(serv_addr.sin_port));
    relacher_verrou(v);
    close(ctx->sockfd);
    free(ctx);
    return NULL;
}

// --- MAIN : RÉCEPTIONNISTE ---

int main() {
    mkdir(TFTP_ROOT, 0755);
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(69),
        .sin_addr        = {INADDR_ANY}
    };
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr));

    printf("╔═══════════════════════════════════╗\n");
    printf("║  SERVEUR TFTP MULTITHREAD (69)    ║\n");
    printf("║  Racine : %-25s║\n", TFTP_ROOT);
    printf("╚═══════════════════════════════════╝\n");
    fflush(stdout);

    while(1) {
        char buf[MAX_PACKET_SIZE];
        struct sockaddr_in c_addr;
        socklen_t c_len = sizeof(c_addr);

        int n = recvfrom(sfd, buf, MAX_PACKET_SIZE, 0,
                         (struct sockaddr*)&c_addr, &c_len);
        if (n < 4) continue;

        uint16_t op = ntohs(*(uint16_t*)buf);
        if (op != TFTP_OP_RRQ && op != TFTP_OP_WRQ) continue;

        tftp_context_t *ctx = malloc(sizeof(tftp_context_t));
        memset(ctx, 0, sizeof(tftp_context_t));
        ctx->peer_addr    = c_addr;
        ctx->peer_len     = c_len;
        ctx->est_ecrivain = (op == TFTP_OP_WRQ);

        strncpy(ctx->filename, buf + 2, sizeof(ctx->filename) - 1);
        char *mode_ptr = buf + 2 + strlen(ctx->filename) + 1;
        strncpy(ctx->mode, mode_ptr, sizeof(ctx->mode) - 1);

        /* Log de la requête entrante */
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &c_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("\n[SERVEUR] %-3s depuis %s:%d → '%s' (mode=%s)\n",
               op == TFTP_OP_RRQ ? "RRQ" : "WRQ",
               ip_str, ntohs(c_addr.sin_port),
               ctx->filename, ctx->mode);
        fflush(stdout);

        pthread_t tid;
        pthread_create(&tid, NULL, traitement_client, ctx);
    }
    return 0;
}