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

void send_request(tftp_context_t *ctx, uint16_t opcode_val) {
    char buffer[MAX_PACKET_SIZE];
    uint16_t *opcode = (uint16_t *)buffer;
    *opcode = htons(opcode_val);
    
    char *p = buffer + 2;
    strcpy(p, ctx->filename);
    p += strlen(ctx->filename) + 1;
    strcpy(p, ctx->mode);
    p += strlen(ctx->mode) + 1;
    
    size_t len = p - buffer;
    sendto(ctx->sockfd, buffer, len, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
}

int verifier_tid(tftp_context_t *ctx, struct sockaddr_in *from) {
    // Premier paquet : mise à jour du TID (port de réponse du serveur)
    if (ctx->last_block == 0 && (ntohs(ctx->peer_addr.sin_port) == 69 || ctx->peer_addr.sin_port == 0)) {
        ctx->peer_addr.sin_port = from->sin_port;
        return 1;
    }
    
    if (from->sin_addr.s_addr != ctx->peer_addr.sin_addr.s_addr || 
        from->sin_port != ctx->peer_addr.sin_port) {
        printf("  [ATTENTION] TID incorrect: %d attendu, %d reçu\n", 
               ntohs(ctx->peer_addr.sin_port), ntohs(from->sin_port));
        return 0;
    }
    return 1;
}

// --- Opérations Principales ---

int tftp_get(tftp_context_t *ctx) {
    ctx->fp = fopen(ctx->filename, "wb");
    if (!ctx->fp) { perror("fopen"); return -1; }

    printf("[GET] Téléchargement de '%s'...\n", ctx->filename);
    send_request(ctx, TFTP_OP_RRQ);
    printf("  -> En attente de prise en charge par le serveur...\n");
    
    int done = 0;
    while (!done) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        char recv_buf[MAX_PACKET_SIZE];
        
        ssize_t n = recvfrom(ctx->sockfd, recv_buf, MAX_PACKET_SIZE, 0, (struct sockaddr *)&from, &from_len);
        
        if (n >= 4) {
            if (!verifier_tid(ctx, &from)) continue;
            
            uint16_t opcode = ntohs(*(uint16_t *)recv_buf);
            uint16_t block = ntohs(*(uint16_t *)(recv_buf + 2));

            if (opcode == TFTP_OP_DATA) {
                if (block == ctx->last_block + 1) {
                    if (ctx->last_block == 0) {
                        printf("  -> Pris en charge par le serveur (TID: %d)\n", ntohs(from.sin_port));
                    }
                    ctx->last_block = block;
                    size_t data_len = n - 4;
                    fwrite(recv_buf + 4, 1, data_len, ctx->fp);
                    
                    tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(block)};
                    sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                    
                    ctx->retries = 0;
                    if (data_len < 512) done = 1;
                } else if (block == ctx->last_block) {
                    tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(block)};
                    sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
                }
            } else if (opcode == TFTP_OP_ERR) {
                printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                fclose(ctx->fp); unlink(ctx->filename); return -1;
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++ctx->retries > TFTP_MAX_RETRIES) { 
                printf("Max retries atteint.\n"); 
                break; 
            }
            printf("[TIMEOUT %d/%d] Pas de réponse au RRQ...\n",ctx->retries, TFTP_MAX_RETRIES);
            if (ctx->last_block == 0) send_request(ctx, TFTP_OP_RRQ);
            else {
                tftp_ack_packet_t ack = {htons(TFTP_OP_ACK), htons(ctx->last_block)};
                sendto(ctx->sockfd, &ack, 4, 0, (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);
            }
        }
    }
    fclose(ctx->fp);
    return 0;
}

int tftp_put(tftp_context_t *ctx) {
    char recv_buf[MAX_PACKET_SIZE];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    // 1. VÉRIFICATION LOCALE (Avant tout échange réseau)
    // On s'assure que le fichier existe et est accessible en lecture
    ctx->fp = fopen(ctx->filename, "rb");
    if (!ctx->fp) { 
        fprintf(stderr, "[ERREUR LOCALE] Impossible d'ouvrir '%s' : %s\n", 
                ctx->filename, strerror(errno));
        return -1; 
    }

    printf("[PUT] Initialisation du transfert pour '%s'...\n", ctx->filename);

    // 2. PHASE DE CONNEXION (WRQ -> ACK 0)
    // Selon la RFC 1350, on doit retransmettre la requête si le serveur ne répond pas
    int connected = 0;
    ctx->retries = 0;
    ctx->last_block = 0; // On attend l'ACK du bloc 0

    while (ctx->retries < TFTP_MAX_RETRIES) {
        send_request(ctx, TFTP_OP_WRQ); // Envoi du paquet WRQ
        if (ctx->retries == 0) {
            printf("  -> En attente d'acceptation du serveur...\n");
        }
        
        ssize_t n = recvfrom(ctx->sockfd, recv_buf, MAX_PACKET_SIZE, 0, 
                             (struct sockaddr *)&from, &from_len);
        
        if (n >= 4) {
            // verifier_tid mettra à jour le port du serveur (TID) lors de la 1ère réponse
            if (verifier_tid(ctx, &from)) {
                uint16_t opcode = ntohs(*(uint16_t *)recv_buf);
                uint16_t block = ntohs(*(uint16_t *)(recv_buf + 2));

                if (opcode == TFTP_OP_ACK && block == 0) {
                    printf("  -> Pris en charge par le serveur (TID: %d)\n", ntohs(from.sin_port));
                    connected = 1;
                    break; // Connexion établie
                } else if (opcode == TFTP_OP_ERR) {
                    printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                    fclose(ctx->fp);
                    return -1;
                }
            }
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Timeout : le serveur n'a peut-être pas reçu le WRQ ou l'ACK 0 est perdu
            ctx->retries++;
            printf("[TIMEOUT %d/%d] Pas de réponse au WRQ...\n",ctx->retries, TFTP_MAX_RETRIES);
        }
    }

    if (!connected) {
        fprintf(stderr, "[ECHEC] Le serveur ne répond pas après %d tentatives.\n", TFTP_MAX_RETRIES);
        fclose(ctx->fp);
        return 2; /* timeout */
    }

    // 3. PHASE DE TRANSFERT DES DONNÉES (DATA -> ACK)
    ctx->last_block = 1;
    uint8_t data_buf[MAX_DATA_SIZE];
    int transfer_done = 0;

    printf("[PUT] Connexion établie. Envoi des données...\n");

    while (!transfer_done) {
        // Lecture d'un bloc dans le fichier
        size_t n_read = fread(data_buf, 1, MAX_DATA_SIZE, ctx->fp);
        
        // Préparation du paquet DATA
        tftp_data_packet_t pkt;
        pkt.opcode = htons(TFTP_OP_DATA);
        pkt.block = htons(ctx->last_block);
        memcpy(pkt.data, data_buf, n_read);

        int acked = 0; 
        ctx->retries = 0;

        // Boucle de retransmission pour ce bloc spécifique
        while (!acked && ctx->retries < TFTP_MAX_RETRIES) {
            sendto(ctx->sockfd, &pkt, 4 + n_read, 0, 
                   (struct sockaddr *)&ctx->peer_addr, ctx->peer_len);

            ssize_t r = recvfrom(ctx->sockfd, recv_buf, MAX_PACKET_SIZE, 0, 
                                 (struct sockaddr *)&from, &from_len);

            if (r >= 4 && verifier_tid(ctx, &from)) {
                uint16_t op = ntohs(*(uint16_t *)recv_buf);
                uint16_t blk = ntohs(*(uint16_t *)(recv_buf + 2));

                if (op == TFTP_OP_ACK && blk == ctx->last_block) {
                    acked = 1; // Bloc acquitté
                } else if (op == TFTP_OP_ERR) {
                    printf("[ERREUR SERVEUR] %s\n", recv_buf + 4);
                    fclose(ctx->fp);
                    return -1;
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ctx->retries++;
                printf("[TIMEOUT] Bloc %d perdu, renvoi (%d/%d)...\n", 
                       ctx->last_block, ctx->retries, TFTP_MAX_RETRIES);
            }
        }

        if (!acked) {
            fprintf(stderr, "[ECHEC] Perte de connexion pendant l'envoi du bloc %d.\n", ctx->last_block);
            fclose(ctx->fp);
            return -1;
        }

        // Si le bloc était plus petit que 512 octets, c'est le dernier bloc (RFC 1350)
        if (n_read < MAX_DATA_SIZE) {
            transfer_done = 1;
        } else {
            ctx->last_block++;
        }
    }

    fclose(ctx->fp);
    printf("[PUT] Transfert de '%s' terminé avec succès.\n", ctx->filename);
    return 0;
}

int main(int argc, char const *argv[]) {
    if (argc < 4) {
        printf("Usage: %s <ip> <get|put> <fichier> [port]\n", argv[0]);
        return 1;
    }

    tftp_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    // Initialisation Contexte
    ctx.sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(ctx.sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ctx.peer_addr.sin_family = AF_INET;
    ctx.peer_addr.sin_port = htons((argc == 5) ? atoi(argv[4]) : 69);
    inet_pton(AF_INET, argv[1], &ctx.peer_addr.sin_addr);
    ctx.peer_len = sizeof(ctx.peer_addr);

    strncpy(ctx.filename, argv[3], sizeof(ctx.filename) - 1);
    strcpy(ctx.mode, "octet");
    ctx.est_ecrivain = 0;

    if (strcmp(argv[2], "get") == 0) 
        tftp_get(&ctx);
    else if (strcmp(argv[2], "put") == 0) 
        tftp_put(&ctx);

    close(ctx.sockfd);
    return 0;
}