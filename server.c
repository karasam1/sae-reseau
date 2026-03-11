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
#include <strings.h>
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

/**
 * @brief Recherche ou crée un verrou pour un fichier donné (opération atomique).
 * Gére la liste globale de verrous pour assurer l'integrité des fichiers.
 */
VerrouFichier* obtenir_verrou(const char *nom) {
    // Section Critique: Accès à la lista globale des verrous.
    pthread_mutex_lock(&mutex_liste);
    VerrouFichier *courant = liste_verrous;

    /* 1. Recherche de verrou dans la liste chaînée. */
    while (courant) {
        if (strcmp(courant->nom_fichier, nom) == 0) {
            courant->ref_count++;
            pthread_mutex_unlock(&mutex_liste);
            return courant;
        }
        courant = courant->suivant;
    }

    /* 2. Créer un nouveau verrou (Première access au fichier) */
    // Si le fichier n'est pas dans ls liste, on alloue dynamiquement une structure de contrôle dediée.
    VerrouFichier *nouveau = malloc(sizeof(VerrouFichier));
    if (!nouveau) { pthread_mutex_unlock(&mutex_liste); return NULL; }

    // Initialisation de mecanismes de synchronisation de verrou.
    memset(nouveau, 0, sizeof(VerrouFichier));
    strncpy(nouveau->nom_fichier, nom, sizeof(nouveau->nom_fichier) - 1);

    // Mutex et variables de condition, indispensables pour implementer le modèle Lecteur/Rédacteur
    pthread_mutex_init(&nouveau->mutex, NULL);
    pthread_cond_init(&nouveau->cond, NULL);
    nouveau->nb_lecteurs  = 0;
    nouveau->en_ecriture  = 0;
    nouveau->ref_count    = 1;
    nouveau->suivant      = liste_verrous; // Insertion en tête de liste.
    liste_verrous         = nouveau;

    // Fin de la section critique globale
    pthread_mutex_unlock(&mutex_liste);
    return nouveau;
}

void envoyer_oack(tftp_context_t *ctx) {
    uint8_t pkt[128];
    uint16_t opcode = htons(TFTP_OP_OACK);
    memcpy(pkt, &opcode, 2);
    int len = sprintf((char*)(pkt + 2), "blksize") + 1;
    len += sprintf((char*)(pkt + 2 + len), "%u", ctx->blksize) + 1;
    sendto(ctx->sockfd, pkt, 2 + len, 0, 
        (struct sockaddr *)&ctx->peer_addr, 
        ctx->peer_len);
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


/**
 * @brief Construit et envoi un paquet DATA TFTP (Opcode 3)
 * * @param ctx Contexte de la session (contient le socket et l'adresse du client).
 * @param block Numéro de bloc de données actuel.
 * @param data Pointeur vers les données brutes à envoyer.
 * @param len Taille des données (doit être <= blksize)
 * @return 0 en cas de succès, -1 en cas d'erreur d'allocation ou d'envoi.
 */
int envoyer_donnees(tftp_context_t *ctx, uint16_t block, const uint8_t *data, size_t len) {
    // Allocation dynamique du paquet.
    // La taille totale est de 4 octets d'en-tête (Opcode + block number)
    // ajoutés à la taille des données utiles (Payload).
    uint8_t *pkt = malloc(4 + len);
    if (!pkt) return -1;

    uint16_t op = htons(TFTP_OP_DATA);
    uint16_t blk = htons(block);

    memcpy(pkt, &op, 2);
    memcpy(pkt + 2, &blk, 2);
    memcpy(pkt + 4, data, len);

    ssize_t sent = sendto(ctx->sockfd, pkt, 4 + len, 0,
        (struct sockaddr *)&ctx->peer_addr, 
        ctx->peer_len);
    
    free(pkt);
    return (sent == (ssize_t)(4 + len)) ? 0 : -1;
}


int verifier_tid(tftp_context_t *ctx, struct sockaddr_in *from) {
    if (ctx->last_block == 0 && ctx->peer_addr.sin_port == 0) {
        ctx->peer_addr.sin_port = from->sin_port;
        ctx->peer_addr.sin_addr = from->sin_addr;
        return 1;
    }
    
    if (from->sin_addr.s_addr != ctx->peer_addr.sin_addr.s_addr || 
        from->sin_port != ctx->peer_addr.sin_port) {
        unsigned long tid = (unsigned long)pthread_self() % 10000;
        // Extraction d'informations de l'instrus
        char *ip_intrus = inet_ntoa(from->sin_addr);
        uint16_t port_intrus = ntohs(from->sin_port);
        
        // Extraction d'informations auprès de clients légitimes à des fins de comparaison
        char *ip_legitime = inet_ntoa(ctx->peer_addr.sin_addr);
        uint16_t port_legitime = ntohs(ctx->peer_addr.sin_port);

        printf("\n[T-%04lu] ⚠️  [ALERTE SÉCURITÉ] Paquet TID invalide détecté !\n", tid);
        printf("          LÉGITIME : %s:%u\n", ip_legitime, port_legitime);
        printf("          INTRUS   : %s:%u\n", ip_intrus, port_intrus);
        printf("          ACTION   : Paquet rejeté silencieusement.\n\n");
        
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

/**
 * @brief Gère une requête de lecture (RQ)
 * Realise l'ouverture sécurisée du fichier, la négociation d'options (OACK)
 * et le transfert par blocs avec gestion des acquittements (ACK).
 */
int executer_rrq(tftp_context_t *ctx) {
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    char chemin[DEFAULT_BLKSIZE];

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("setsockopt timeout");
    }

    // 1. Securisation de chemin d'accès. (Cela prévient les attaques de type 'Directory Traversal')
    if (!construire_chemin_securise(chemin, sizeof(chemin), ctx->filename)) {
        printf("[T-%04lu] [ERREUR] Chemin invalide : '%s'\n", tid, ctx->filename);
        envoyer_erreur(ctx, ERR_ACCESS_VIOLATION, "Chemin invalide");
        return -1;
    }

    // 2. Ouverture du fichier source, en mode rb: Read Binary, pour garantir l'integrité des données 
    // (mode octect RFC 1350).
    ctx->fp = fopen(chemin, "rb");
    if (!ctx->fp) {
        printf("[T-%04lu] [ERREUR] Fichier introuvable : '%s' (%s)\n",
            tid, ctx->filename, strerror(errno));
        envoyer_erreur(ctx, ERR_FILE_NOT_FOUND, strerror(errno));
        return -1;
    }

    // 3. Negociation des options (RFC 2347/2348)
    if (ctx->blksize != DEFAULT_BLKSIZE) {
        envoyer_oack(ctx);
        uint8_t temp_ack[16];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t r = recvfrom(ctx->sockfd, temp_ack, sizeof(temp_ack), 0, (struct sockaddr *)&from, &from_len);
        if (r < 4) { fclose(ctx->fp); return -1; }
        uint16_t op = ntohs(*(uint16_t *)temp_ack);
        uint16_t blk = ntohs(*(uint16_t *)(temp_ack + 2));
        if (op != TFTP_OP_ACK || blk != 0) {
            fclose(ctx->fp); return -1; 
        }
    }   

    printf("[T-%04lu] RRQ '%s' → envoi en cours...\n", tid, ctx->filename);
    fflush(stdout);

    uint32_t current_block = 1;
    uint8_t *buffer_dynamique = malloc(ctx->blksize);
    uint8_t ack_buf[16];
    int done = 0;

// dans executer_rrq()
    while (!done) {
        // Logique de detection
        size_t n = fread(buffer_dynamique, 1, ctx->blksize, ctx->fp);
        ctx->retries = 0;
        int acked = 0;

        while (!acked && ctx->retries < TFTP_MAX_RETRIES) {
            envoyer_donnees(ctx,(uint16_t) current_block, buffer_dynamique, n);

            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t r = recvfrom(ctx->sockfd, ack_buf, sizeof(ack_buf), 0,
                               (struct sockaddr *)&from, &from_len);

            if (r >= 4 && verifier_tid(ctx, &from)) {
                uint16_t op = ntohs(*(uint16_t *)ack_buf);
                uint16_t blk_received = ntohs(*(uint16_t *)(ack_buf + 2));
                if (op == TFTP_OP_ACK && blk_received == (uint16_t)current_block) {
                    acked = 1;
                    ctx->last_block = blk_received;
                }

            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                printf("[T-%04lu] [TIMEOUT %d/%d] RRQ '%s' bloc %u\n",
                       tid, ++ctx->retries, TFTP_MAX_RETRIES,
                       ctx->filename, current_block);
                fflush(stdout);
            }
        }
        if (!acked) {
            if (n < ctx->blksize) {
                printf("[T-%04lu] Transfert terminé (Dernier ACK non reçu, mais fichier envoyé en totalité).\n", tid);
                fclose(ctx->fp);
                free(buffer_dynamique);
                return 0; 
            } else {
                printf("[T-%04lu] [ECHEC] RRQ '%s' - max retries atteint\n", tid, ctx->filename);
                fclose(ctx->fp);
                free(buffer_dynamique);
                return -1;
            }
        }
        if (n < ctx->blksize) done = 1; // Contidion de rupture
        else current_block++;
    }
    fclose(ctx->fp); // Cloture de la Session
    free(buffer_dynamique);
    printf("[T-%04lu] RRQ '%s' → %u bloc(s) envoyé(s) ✓\n", tid, ctx->filename, current_block);
    fflush(stdout);
    return 0;
}

int executer_wrq(tftp_context_t *ctx) {
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    char chemin[DEFAULT_BLKSIZE];
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
    if (ctx->blksize != DEFAULT_BLKSIZE) {
        envoyer_oack(ctx);
    } else {
        envoyer_ack(ctx, 0);
    }
    printf("[T-%04lu] WRQ '%s' → réception en cours...\n", tid, ctx->filename);
    fflush(stdout);

    ctx->last_block = 0;
    uint8_t *buf_pkt = malloc(ctx->blksize + 4);
    uint32_t expected_block = 1;
    int done = 0;

    while (!done) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(ctx->sockfd, buf_pkt, ctx->blksize + 4, 0,
                             (struct sockaddr *)&from, &from_len);
        if (n < 4) continue;
        if (!verifier_tid(ctx, &from)) continue;

        uint16_t op    = ntohs(*(uint16_t *)buf_pkt);
        uint16_t block_received = ntohs(*(uint16_t *)(buf_pkt + 2));

        if (op == TFTP_OP_DATA) {
            if (block_received == (uint16_t)expected_block) {
                size_t data_len = n - 4;
                size_t written = fwrite(buf_pkt + 4, 1, data_len, ctx->fp);
                if (written < data_len) {
                    printf("[T-%04lu] [ERREUR] Disque plein lors de l'écriture de '%s'\n", tid, ctx->filename);
                    envoyer_erreur(ctx, ERR_DISK_FULL, "Erreur disque");
                    fclose(ctx->fp); 
                    unlink(chemin);
                    free(buf_pkt);
                    return -1;
                }
                envoyer_ack(ctx, block_received);
                ctx->last_block = block_received;
                if (data_len < (size_t)ctx->blksize) done = 1;
                else expected_block++;
            } else if (block_received == (uint16_t)(expected_block - 1)) {
                envoyer_ack(ctx, block_received);
            }
        }
    }
    free(buf_pkt);
    fclose(ctx->fp);
    printf("[T-%04lu] WRQ '%s' → %u bloc(s) reçu(s) ✓\n",
           tid, ctx->filename, (uint32_t)expected_block);
    fflush(stdout);
    return 0;
}

// --- LOGIQUE DES THREADS OUVRIERS ---

void* traitement_client(void* arg) {
    tftp_context_t *ctx = (tftp_context_t*)arg;

    // Generation d'un ID court pour identifier ce thread dans les logs
    unsigned long tid = (unsigned long)pthread_self() % 10000;
    pthread_detach(pthread_self()); // Detachemment de thread, liberation des ressources à sa terminaison.

    /* 1. Creation du TID (transfert identifier) - RFC 1350 */
    // Selon le standard, chaque transfert doit utiliser un nouveau port UDP pour
    // liberer le port 69 et isoler les flux de données.
    ctx->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->sockfd < 0) {
        printf("[T-%04lu] [ERREUR] Impossible de créer la socket TID\n", tid);
        free(ctx); return NULL;
    }

    // Liaison à un port éphémère.
    // En fixant sin_port à zero, l'OS choisit automatiquement un port libre.
    // Ce nouveau port devient le TID officiel du serveur pour cette session.
    struct sockaddr_in serv_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = 0
    };
    if (bind(ctx->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[T-%04lu] [ERREUR] Bind TID échoué\n", tid);
        close(ctx->sockfd); free(ctx); return NULL;
    }

    // Configuration du délai d'attente.
    // Essentiel pour la fiabilité TFTP: si le client ne repond pas.
    // recvfrom() sortira en erreur après TFTP_TIMEOUT_SEC
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 2. Gestion de l'exclusion mutuelle (File Locking) */
    // Avant d'acceder au fichier, on récupère un verrou pour éviter
    // pour eviter les conditions de concurrence (ex: deux clients écrivant dans le même fichier).
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
    // Initialisation de l'arborisation du serveur.
    mkdir(TFTP_ROOT, 0755); // Creation de réportorie racine avec les droits 0755
    
    // Instantiation du socket de contrôle 
    // IF_INET : Protocole IPV4
    // SOCK_DIAGRAM : TFTP repose exlusivement sur UDP (RFC 1350)
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);

    // Configuration de l'addresse d'écoute.
    // sin_port : Port 69 (port standard "Well-known" pour TFTP)
    // sin_addr : INADDR_ANY accepte toutes les conexions sur toutes les interfaces réseau.
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(69),
        .sin_addr        = {INADDR_ANY}
    };

    // Liaison du socket à l'interface réseau.
    bind(sfd, (struct sockaddr *)&addr, sizeof(addr)); // Note: L'éxecution sur le port 69 nécessite généralement des privilegies 'root' 

    printf("╔═══════════════════════════════════╗\n");
    printf("║  SERVEUR TFTP MULTITHREAD (69)    ║\n");
    printf("║  Racine : %-24s║\n", TFTP_ROOT);
    printf("╚═══════════════════════════════════╝\n");
    
    // Force le vidage de sortie pour un long en temps réel.
    fflush(stdout);

    while(1) {
        char buf[MAX_PACKET_SIZE];
        struct sockaddr_in c_addr;
        socklen_t c_len = sizeof(c_addr);

        // Attente synchrone d'un datagrame UDP
        // sfd : socket d'ecoute sur le port 69.
        // c_addr : Capturera l'addresse IP et le port source du client (TID initial).
        int n = recvfrom(sfd, buf, MAX_PACKET_SIZE, 0,
                         (struct sockaddr*)&c_addr, &c_len);
        
        // Validation minimale du paquet
        // Un paquet TFTP doit contenir au moins un opcode (2 octects).
        // Un nom de fichier (min 1 octect) et un délimiteur nul
        if (n < 4) continue;

        // Analyse de l'Opcode (format réseau vers hôte)
        uint16_t op = ntohs(*(uint16_t*)buf); // Seul requêtes RRQ et WRQ sont acceptes sur le port 69 (RFC 1350)
        if (op != TFTP_OP_RRQ && op != TFTP_OP_WRQ) continue;

        // Allocation dynamique de le contexte de session.
        // Chaque requête entrante génère un nouveau contexte 'ctx' pour isoler les donées
        // de la session et permettre un traitement multithread sécurisé.
        tftp_context_t *ctx = malloc(sizeof(tftp_context_t));
        memset(ctx, 0, sizeof(tftp_context_t));
        ctx->peer_addr    = c_addr;
        ctx->peer_len     = c_len;
        ctx->est_ecrivain = (op == TFTP_OP_WRQ);
        ctx->blksize = MAX_PACKET_SIZE;

        // Extraction du nom de fichier, utilisation du strcnpy pour prevenir des desbordements de tampon.
        strncpy(ctx->filename, buf + 2, sizeof(ctx->filename) - 1);
        char *current_ptr = buf + 2 + strlen(ctx->filename) + 1;

        // Extraction du mode de fichier
        strncpy(ctx->mode, current_ptr, sizeof(ctx->mode) - 1);
        current_ptr +=strlen(ctx->mode) + 1;

        // Parsing des options d'extension (RFC 2347) 
        while (current_ptr < (buf + n)) {
            // Recherche de l'option blksize (RFC 2348)
            if (strcasecmp(current_ptr, "blksize") == 0) {
                current_ptr += strlen(current_ptr) + 1;

                if (current_ptr < (buf + n)) {
                    int requested_blksize = atoi(current_ptr);

                    // Validationn de la taille de bloc demandée.
                    if (requested_blksize >= 8 && requested_blksize <= MAX_BLKSIZE) {
                        ctx->blksize = (uint16_t)requested_blksize;
                    }
                    current_ptr += strlen(current_ptr) + 1;
                }
            } else {
                // Option inconnue: Ignorer proprement.
                // Selon le RFC 2347, si une option n'est pas suportée, 
                // le serveur doit simplement l'ignorer et ne pas l'inclure dans l'OACK.
                current_ptr += strlen(current_ptr) + 1; // Saute le clé
                if (current_ptr < (buf + n)) current_ptr += strlen(current_ptr) + 1; // saute le valeur
            }
        }
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &c_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("\n[SERVEUR] %-3s depuis %s:%d → '%s' (mode=%s, blksize=%u)\n",
            op == TFTP_OP_RRQ ? "RRQ" : "WRQ",
            ip_str, 
            ntohs(c_addr.sin_port),
            ctx->filename, 
            ctx->mode,
            ctx->blksize);
        fflush(stdout);

        pthread_t tid;
        pthread_create(&tid, NULL, traitement_client, ctx);
    }
    return 0;
}
