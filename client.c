#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include "tftp_protocole.h"
#include "tftp_errors.h"


// --- Fonctions Helper ---


/**
 * @brief: Envoie une requête RRQ/WRQ avec extension d'options (RFC 2347).
 * * Construit un paquet TFTP client incluant l'opcode (RFC 1350), le nom du fichier,
 * le mode de transfert, et l'option "blksize" pour la négotiation de la taille
 * de bloc (RFC 2348).
 * * @param ctx Le contexte de la session TFTP
 * @param opcode_val L'opcode (1 pour RRQ, 2 pour WRQ)
 */
void send_request(tftp_context_t *ctx, uint16_t opcode_val) {
    char buffer[MAX_PACKET_SIZE];

    // Positionnement de l'opcode (Format réseau) - 1350
    uint16_t *opcode = (uint16_t *)buffer;
    *opcode = htons(opcode_val);
    
    char *p = buffer + 2;
    
    // Copie du nom de fichier et du mode (Netascii, Octect, Mail)
    // Note: Une vérification de la taille de ctx->filename est préconeisée ici.
    strcpy(p, ctx->filename);
    p += strlen(ctx->filename) + 1;
    
    strcpy(p, ctx->mode);
    p += strlen(ctx->mode) + 1;
    
    // Négociation de l'option Block Size - RFC 2347 & RFC 2348
    // blksize permet de dépasser la limite par défaut de 512 octects.
    strcpy(p, "blksize");
    p += strlen("blksize") + 1;

    // On demande une taille optimisée pour éviter la fragmentation IP
    sprintf(p, "%d", MAX_BLKSIZE);
    p += strlen(p) + 1;

    // Envoi du datagramme vers le serveur distant
    size_t len = p - buffer;
    sendto(ctx->sockfd, buffer, len, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
}

/**
 * @brief Verifie l'identite del emetteur (TID) conformement au RFC 1350.
 ** @param ctx Contexte actuel de la session TFTP.
 * @param from Addresse source de paquet reçue.
 * @return 1 si le TID es valide, sinon non.
 */
int verifier_tid(tftp_context_t *ctx, struct sockaddr_in *from) {
    /* * 1. Phase de la capture du TID (Premier paquet)
     * Enregistrer le port éphèmère choisi par le serveur
    */
    if (ctx->last_block == 0 && (ntohs(ctx->peer_addr.sin_port) == 69 || ctx->peer_addr.sin_port == 0)) {
        ctx->peer_addr.sin_port = from->sin_port;
        return 1;
    }
    
    // 2. Verification de la coherence de la source (Anti-Spoofing/Isolation)
    if (from->sin_addr.s_addr != ctx->peer_addr.sin_addr.s_addr || 
        from->sin_port != ctx->peer_addr.sin_port) {
        // Journalisation de l'anomalie de sécurité/réseau.
        printf("  [ATTENTION] TID incorrect: %d attendu, %d reçu\n", 
               ntohs(ctx->peer_addr.sin_port), ntohs(from->sin_port));
        return 0;
    }
    return 1;
}

void envoyer_erreur(tftp_context_t *ctx, uint16_t code, const char *msg) {
    size_t len = strlen(msg);
    // Taille: Opcode(2) + ErrorCode(2) + Mensaje(n) + Zero(1)
    size_t packet_len = 4 + len + 1;
    uint8_t *packet = malloc(packet_len);
    if (!packet) return;

    uint16_t opcode = htons(TFTP_OP_ERR);
    uint16_t err_code = htons(code);

    memcpy(packet, &opcode, 2);
    memcpy(packet + 2, &err_code, 2);
    strcpy((char *)packet + 4, msg);

    sendto(ctx->sockfd, packet, packet_len, 0, 
           (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
    
    printf("  [CLIENT] Erreur envoyé au serveur: %s (Code %d)\n", msg, code);
    free(packet);
}

// --- Opérations Principales ---


/**
 * @brief Implémente le transfert RRQ (Read Request) - RFC 1350 & 2347
 * * Gére le cycle de vie du téléchargement:
 * 1. Envoi une requête initiale.
 * 2. Négociation des options (OACK).
 * 3. Réception itérative de DATA et acquittement (ACK).
 * 4. Gestion des rettransmissions pour le timeout.
 * 5. Nettoyage et clotûre propre (Dallying).
*/
int tftp_get(tftp_context_t *ctx) {

    // Ouverture du fichier local en mode "Write Binary" (WB)
    ctx->fp = fopen(ctx->filename, "wb");
    if (!ctx->fp) { perror("fopen"); return -1; }

    // Initilisation des parametres de contrôle de flux
    ctx->blksize = DEFAULT_BLKSIZE; // 512 par défaut (RFC 1350), sera mis à jour si un OACK est reçu.
    ctx->last_block = 0;
    uint32_t expected_block = 1; // Le premier bloc de données est toujours le n°1.
    int done = 0;
    int success = 0;

    printf("[GET] Téléchargement de '%s'...\n", ctx->filename);
    
    // Allocation du tampon de réception sur le tas (heap)
    uint8_t *recv_buf = malloc(2048); // On alloue 2048 octects pour accueillir confortablement l'en-tête (4 octects)
                                      // et les données (blksize jusqu'à 1432 octects), evitant tout depassement.
    if (!recv_buf) { fclose(ctx->fp); return -1; }

    // Amorçage du protocole: Envoi du paquet Read Request (RRQ)
    send_request(ctx, TFTP_OP_RRQ); // Construction du paquet avec l'opcode 1 et les options (RFC 2347)
    
    while (!done) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(ctx->sockfd, recv_buf, 2048, 0, (struct sockaddr *)&from, &from_len);
        if (n >= 4) {
            if (!verifier_tid(ctx, &from)) continue;
            
            uint16_t opcode = ntohs(*(uint16_t *)recv_buf);
            uint16_t block_rx = ntohs(*(uint16_t *)(recv_buf + 2));

            if (opcode == TFTP_OP_OACK) {
                char *ptr = (char*)recv_buf + 2;
                while (ptr < (char*)recv_buf + n) {
                    if (strcasecmp(ptr, "blksize") == 0) {
                        ctx->blksize = atoi(ptr + strlen(ptr) + 1);
                        break;
                    }
                    ptr += strlen(ptr) + 1;
                }
                tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(0)};
                sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                ctx->retries = 0;
                continue;
            }

            if (opcode == TFTP_OP_DATA) {
                if (block_rx == (uint16_t)expected_block) {
                    size_t data_len = n - 4;
                    
                    if (fwrite(recv_buf + 4, 1, data_len, ctx->fp) < data_len) {
                        envoyer_erreur(ctx, 3, "Disk full or write error");
                        done = 1; 
                        success = 0;
                        continue;
                    }
                    
                    tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(block_rx)};
                    sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                    
                    ctx->last_block = block_rx;
                    ctx->retries = 0;
                    
                    if (data_len < (size_t)ctx->blksize) {
                        done = 1;
                        success = 1;
                    } else {
                        expected_block++;
                    }
                } 
                else if (block_rx == (uint16_t)(expected_block - 1)) {
                    tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(block_rx)};
                    sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                }
            } 
            else if (opcode == TFTP_OP_ERR) {
                printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                done = 1;
                success = 0;
            }
        } 
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++ctx->retries > TFTP_MAX_RETRIES) { 
                printf("[ECHEC] Max retries atteint. Le serveur ne répond plus.\n"); 
                done = 1;
                success = 0;
            } else {
                printf("[TIMEOUT %d/%d] Renvoi du dernier ACK...\n", ctx->retries, TFTP_MAX_RETRIES);
                if (ctx->last_block == 0) {
                    send_request(ctx, TFTP_OP_RRQ);
                } else {
                    tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(ctx->last_block)};
                    sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                }
            }
        }
    }

    if (success) {
        int final_retries = 0;
        tftp_ack_packet_t final_ack = {htons(TFTP_OP_ACK), htons(ctx->last_block)};
        
        while (final_retries < 3) {
            struct timeval tv_short = {0, 500000}; 
            setsockopt(ctx->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_short, sizeof(tv_short));

            ssize_t r = recvfrom(ctx->sockfd, recv_buf, 2048, 0, NULL, NULL);
            if (r >= 4) {
                uint16_t opcode = ntohs(*(uint16_t *)recv_buf);
                if (opcode == TFTP_OP_DATA) {
                    sendto(ctx->sockfd, &final_ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                    final_retries++;
                } else {
                    break;
                }
            } else {
                break; 
            }
        }
    }

    free(recv_buf);
    fclose(ctx->fp);

    if (!success) {
        unlink(ctx->filename);
        return -1;
    }

    printf("[GET] Téléchargement de '%s' terminé avec succès ✓\n", ctx->filename);
    return 0;
}

ssize_t envoyer_donnees(tftp_context_t *ctx, uint16_t block, uint8_t *data, size_t data_len) {
    size_t packet_len = 4 + data_len;
    uint8_t *packet = malloc(packet_len);
    if (!packet) return -1;

    uint16_t opcode = htons(TFTP_OP_DATA);
    uint16_t block_n = htons(block);
    
    memcpy(packet, &opcode, 2);
    memcpy(packet + 2, &block_n, 2);
    
    if (data_len > 0) {
        memcpy(packet + 4, data, data_len);
    }

    ssize_t sent = sendto(ctx->sockfd, packet, packet_len, 0, 
                          (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
    
    free(packet);
    return sent;
}

int tftp_put(tftp_context_t *ctx) {
    char recv_buf[MAX_PACKET_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int connected = 0;

    // 1. VÉRIFICATION LOCALE
    ctx->fp = fopen(ctx->filename, "rb");
    if (!ctx->fp) { 
        fprintf(stderr, "[ERREUR LOCALE] Impossible d'ouvrir '%s' : %s\n", 
                ctx->filename, strerror(errno));
        return -1; 
    }

    printf("[PUT] Initialisation du transfert pour '%s'...\n", ctx->filename);

    // 2. PHASE DE CONNEXION (WRQ -> ACK 0 ou OACK)
    ctx->retries = 0;
    ctx->blksize = DEFAULT_BLKSIZE; // Valeur par défaut si pas de négociation

    while (ctx->retries < TFTP_MAX_RETRIES) {
        send_request(ctx, TFTP_OP_WRQ); 
        
        if (ctx->retries == 0) {
            printf("  -> En attente d'acceptation du serveur...\n");
        }
        
        ssize_t n = recvfrom(ctx->sockfd, recv_buf, MAX_PACKET_SIZE, 0, 
                             (struct sockaddr *)&from, &from_len);
        
        if (n >= 4) {
            if (verifier_tid(ctx, &from)) {
                uint16_t opcode = ntohs(*(uint16_t *)recv_buf);
                uint16_t block = ntohs(*(uint16_t *)(recv_buf + 2));

                if (opcode == TFTP_OP_ACK && block == 0) {
                    printf("  -> Pris en charge (ACK 0)\n");
                    connected = 1;
                    break;
                } else if (opcode == TFTP_OP_OACK) {
                    // Analyse simple de l'option blksize dans l'OACK
                    char *ptr = (char *)recv_buf + 2;
                    while (ptr < (char *)recv_buf + n) {
                        if (strcasecmp(ptr, "blksize") == 0) {
                            ctx->blksize = atoi(ptr + strlen(ptr) + 1);
                            break;
                        }
                        ptr += strlen(ptr) + 1;
                    }
                    printf("  -> Pris en charge (OACK: blksize=%d)\n", ctx->blksize);
                    connected = 1;
                    break;
                } else if (opcode == TFTP_OP_ERR) {
                    printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                    fclose(ctx->fp);
                    return -1;
                }
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ctx->retries++;
            printf("[TIMEOUT %d/%d] Pas de réponse au WRQ...\n", ctx->retries, TFTP_MAX_RETRIES);
        }
    }

    if (!connected) {
        fprintf(stderr, "[ECHEC] Le serveur ne répond pas.\n");
        fclose(ctx->fp);
        return -1;
    }

    // 3. PHASE DE TRANSFERT DES DONNÉES (DATA -> ACK)
    uint32_t current_block = 1; // 32 bits pour le rollover
    uint8_t *data_buf = malloc(ctx->blksize);
    int transfer_done = 0;

    printf("[PUT] Connexion établie. Envoi des données...\n");

    

    while (!transfer_done) {
        size_t n_read = fread(data_buf, 1, ctx->blksize, ctx->fp);
        
        if (ferror(ctx->fp)) {
            envoyer_erreur(ctx, 0, "Local file read error");
            free(data_buf); 
            fclose(ctx->fp);
            return -1;
        }

        int acked = 0; 
        ctx->retries = 0;

        while (!acked && ctx->retries < TFTP_MAX_RETRIES) {
            // Utilise la fonction envoyer_donnees dynamique
            envoyer_donnees(ctx, (uint16_t)current_block, data_buf, n_read);

            ssize_t r = recvfrom(ctx->sockfd, recv_buf, MAX_PACKET_SIZE, 0, 
                                 (struct sockaddr *)&from, &from_len);

            if (r >= 4 && verifier_tid(ctx, &from)) {
                uint16_t op = ntohs(*(uint16_t *)recv_buf);
                uint16_t blk = ntohs(*(uint16_t *)(recv_buf + 2));

                if (op == TFTP_OP_ACK && blk == (uint16_t)current_block) {
                    acked = 1; 
                } else if (op == TFTP_OP_ERR) {
                    printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                    free(data_buf); fclose(ctx->fp);
                    return -1;
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ctx->retries++;
                printf("[TIMEOUT] Bloc %u perdu, renvoi (%d/%d)...\n", 
                       (uint16_t)current_block, ctx->retries, TFTP_MAX_RETRIES);
            }
        }

        if (!acked) {
            fprintf(stderr, "[ECHEC] Perte de connexion.\n");
            free(data_buf); fclose(ctx->fp);
            return -1;
        }

        if (n_read < (size_t)ctx->blksize) {
            transfer_done = 1;
        } else {
            current_block++;
        }
    }

    free(data_buf);
    fclose(ctx->fp);
    printf("[PUT] Transfert de '%s' terminé avec succès (%u blocs).\n", ctx->filename, current_block);
    return 0;
}


/**
 * @brief Point d'entrée du client TFTP (RFC 1350)
 * * Gere l'initialisation du socket UDP, la configuration du timeout de reception.
 * et l'affichage vers les operations de lecture (RRQ) ou d'écriture (WRQ).
*/
int main(int argc, char const *argv[]) {

    // Vérification des arguments de la ligne de commande
    if (argc < 4) {
        printf("Usage: %s <ip> <get|put> <fichier> [port]\n", argv[0]);
        return 1;
    }

    tftp_context_t ctx;
    memset(&ctx, 0, sizeof(ctx)); // Misé à zero pour eviter les données résiduels.

    // Creation du socket UDP
    ctx.sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    // Configuration du timeout pour éviter le blocage infini en cas de perte de paquet
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(ctx.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Configuration de l'addresse de destination
    ctx.peer_addr.sin_family = AF_INET;
    // Port 69 par défaut selon le standard, ou port personalisé
    ctx.peer_addr.sin_port = htons((argc == 5) ? atoi(argv[4]) : 69);
    inet_pton(AF_INET, argv[1], &ctx.peer_addr.sin_addr);
    ctx.peer_len = sizeof(ctx.peer_addr);

    // Initialisation des paramètres de la requête
    strncpy(ctx.filename, argv[3], sizeof(ctx.filename) - 1);
    strcpy(ctx.mode, "octet"); // Mode binaire pour l'integrité des fichiers
    ctx.est_ecrivain = 0;

    // Aiguillage vers la fonction spécifique (RFC 1350)
    if (strcmp(argv[2], "get") == 0) 
        tftp_get(&ctx);
    else if (strcmp(argv[2], "put") == 0) 
        tftp_put(&ctx);

    // Liberation des ressources système
    close(ctx.sockfd);
    return 0;
}