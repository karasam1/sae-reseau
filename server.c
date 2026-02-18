#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_FILES 128

typedef struct {
    char filename[256];
    pthread_mutex_t mutex;
    bool in_use;
} file_mutex_t;

file_mutex_t file_mutexes[MAX_FILES];
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;

file_mutex_t* get_file_mutex(const char* filename) {
    pthread_mutex_lock(&global_mutex);
    for (int i = 0; i < MAX_FILES; ++i) {
        if (file_mutexes[i].in_use && strcmp(file_mutexes[i].filename, filename) == 0) {
            pthread_mutex_unlock(&global_mutex);
            return &file_mutexes[i];
        }
    }
    // Not found, create new
    for (int i = 0; i < MAX_FILES; ++i) {
        if (!file_mutexes[i].in_use) {
            strncpy(file_mutexes[i].filename, filename, 255);
            file_mutexes[i].filename[255] = '\0';
            pthread_mutex_init(&file_mutexes[i].mutex, NULL);
            file_mutexes[i].in_use = true;
            pthread_mutex_unlock(&global_mutex);
            return &file_mutexes[i];
        }
    }
    pthread_mutex_unlock(&global_mutex);
    return NULL;
}

#define REPOSITORY ".tftp/"
#define PORT 69
#define MAX_BUF 516
#define TFTP_TIMEOUT_SEC 5
#define TFTP_MAX_ESSAI 5

void send_error(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, uint16_t err_code, const char *err_msg) {
    char err_packet[MAX_BUF];
    uint16_t opcode = htons(5); 
    uint16_t error_code = htons(err_code); 
    memcpy(err_packet, &opcode, 2);
    memcpy(err_packet + 2, &error_code, 2);
    int len = 4 + sprintf(err_packet + 4, "%s", err_msg) + 1;
    sendto(sockfd, err_packet, len, 0, (struct sockaddr *)client_addr, addr_len);
}

void* thread_rrq(void* arg) {
    struct {
        struct sockaddr_in client_addr;
        socklen_t addr_len;
        char fichier[516];
    } *params = arg;
    traitement_rrq(&params->client_addr, params->addr_len, params->fichier);
    free(params);
    return NULL;
}

void* thread_wrq(void* arg) {
    struct {
        struct sockaddr_in client_addr;
        socklen_t addr_len;
        char fichier[516];
    } *params = arg;
    traitement_wrq(&params->client_addr, params->addr_len, params->fichier);
    free(params);
    return NULL;
}

void traitement_rrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); 
    
    // Configurer le timeout sur la socket de transfert
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0}; 
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Le paramètre 'fichier' contient: <filename>\0<mode>\0 selon RFC
    const char *filename = fichier;
    const char *mode = fichier + strlen(filename) + 1;
    if (!filename || !mode) {
        send_error(sockfd, client_addr, addr_len, 4, "Illegal TFTP operation");
        close(sockfd);
        return;
    }

    // Supporter uniquement le mode "octet" pour l'instant
    if (strcasecmp(mode, "octet") != 0) {
        printf("  [SERVER] Mode non supporte: %s\n", mode);
        send_error(sockfd, client_addr, addr_len, 4, "Illegal TFTP operation");
        close(sockfd);
        return;
    }

    if (strstr(filename, "..")) {
        printf("  [SERVER] Erreur : Tentative d'accès non autorisé '%s'.\n", filename);
        send_error(sockfd, client_addr, addr_len, 2, "Access violation");
        close(sockfd);
        return;
    }

    char chemin[256];
    snprintf(chemin, sizeof(chemin), REPOSITORY "%s", filename);
    file_mutex_t* mtx = get_file_mutex(filename);
    if (mtx) pthread_mutex_lock(&mtx->mutex);
    FILE *f = fopen(chemin, "rb");
    
    if (!f) {
        printf("  [SERVER] Erreur : Fichier '%s' introuvable.\n", chemin);
        send_error(sockfd, client_addr, addr_len, 1, "File not found");
        close(sockfd);
        return;
    }

    char buffer[MAX_BUF];
    char ack_buf[4];
    uint16_t block_num = 1;
    size_t read_len;

    do {
        uint16_t opcode = htons(3);
        uint16_t block = htons(block_num);
        memcpy(buffer, &opcode, 2);
        memcpy(buffer + 2, &block, 2);
        read_len = fread(buffer + 4, 1, 512, f);

        int tentatives = 0;
        int ack_recu = 0;
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);

        // Boucle de retransmission pour le bloc actuel
        while (tentatives < TFTP_MAX_ESSAI && !ack_recu) {
            if (sendto(sockfd, buffer, read_len + 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) {
                perror("sendto");
                break;
            }
                printf("  [GET] Envoi du bloc %d (%zu octets)...\n", block_num, read_len);

            ssize_t r = recvfrom(sockfd, ack_buf, 4, 0, (struct sockaddr *)&peer_addr, &peer_len);
            if (r >= 4) {
                // Si paquet provenant d'une source inattendue, envoyer ERROR(5) "Unknown transfer ID" (RFC)
                if (peer_addr.sin_addr.s_addr != client_addr->sin_addr.s_addr || peer_addr.sin_port != client_addr->sin_port) {
                    printf("  [WARNING] Paquet inattendu depuis %s:%d (attendu %s:%d) -> envoi ERROR(5)\n",
                        inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port),
                        inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
                    send_error(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                    continue; // attendre le bon paquet
                }
                uint16_t ack_val = ntohs(*(uint16_t *)(ack_buf + 2));
                if (ack_val == block_num) {
                    ack_recu = 1; // ACK valide reçu !
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
                printf("  [TIMEOUT] Pas d'ACK pour bloc %d, tentative %d/%d...\n", block_num, tentatives, TFTP_MAX_ESSAI);
            } else {
                if (r < 0) perror("recvfrom");
                break;
            }
        }

        if (!ack_recu) break; // On arrête tout si le client ne répond plus
        block_num++;
    } while (read_len == 512);

    printf("  [GET] Transfert de '%s' terminé.\n", fichier);
    fclose(f);
    if (mtx) pthread_mutex_unlock(&mtx->mutex);
    close(sockfd);
}

void traitement_wrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    // Configurer le timeout
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 'fichier' contient: <filename>\0<mode>\0
    const char *filename = fichier;
    const char *mode = fichier + strlen(filename) + 1;
    if (!filename || !mode) {
        send_error(sockfd, client_addr, addr_len, 4, "Illegal TFTP operation");
        close(sockfd);
        return;
    }

    // Supporter uniquement le mode "octet" pour l'instant
    if (strcasecmp(mode, "octet") != 0) {
        printf("  [SERVER] Mode non supporte: %s\n", mode);
        send_error(sockfd, client_addr, addr_len, 4, "Illegal TFTP operation");
        close(sockfd);
        return;
    }

    file_mutex_t* mtx = get_file_mutex(filename);
    if (mtx) pthread_mutex_lock(&mtx->mutex);
    char *buffer_final = NULL;
    size_t taille_totale = 0;
    uint16_t dernier_block_recu = 0;
    
    // ACK 0 initial
    char ack[4] = {0, 4, 0, 0};
    if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) perror("sendto");
    printf("  [PUT] ACK 0 envoyé\n");

    char buffer_reception[MAX_BUF];
    ssize_t n;
    printf("  [PUT] Réception de '%s'...\n", filename);

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int peer_set = 0;
    int recu_ok = 0;

    do {
        int tentatives = 0;

        while (tentatives < TFTP_MAX_ESSAI && !recu_ok) {
            ssize_t r = recvfrom(sockfd, buffer_reception, MAX_BUF, 0, (struct sockaddr *)&peer_addr, &peer_len);

            if (r >= 4) {
                // si peer non défini, on l'enregistre
                if (!peer_set) 
                    peer_set = 1;
                // Vérification stricte du TID (Transfer Identifier) : IP et Port doivent correspondre
                if (peer_addr.sin_addr.s_addr != client_addr->sin_addr.s_addr || peer_addr.sin_port != client_addr->sin_port) {
                    printf("  [WARNING] Paquet inattendu depuis %s:%d (attendu %s:%d) -> envoi ERROR(5)\n",
                        inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port),
                        inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
                    send_error(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                    continue;
                }

                uint16_t block_recu = ntohs(*(uint16_t *)(buffer_reception + 2));
                if (block_recu == dernier_block_recu + 1) {
                    recu_ok = 1; // Nouveau bloc reçu
                    n = r;
                } else if (block_recu == dernier_block_recu) {
                    // Doublon : on renvoie juste l'ACK sans traiter les données
                    if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len) < 0) 
                        perror("sendto");
                    // ou sinon paquet hors séquence -> on ignore
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
                printf("  [TIMEOUT] Attente bloc %d, tentative %d/%d... Renvoi dernier ACK.\n", dernier_block_recu + 1, tentatives, TFTP_MAX_ESSAI);
                // renvoyer le dernier ACK connu
                if (peer_set) {
                    if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len) < 0) perror("sendto");
                } else {
                    if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) perror("sendto");
                }
            } else {
                if (r < 0) perror("recvfrom");
                break;
            }
        }

        if (!recu_ok) break;

        // Traitement des données reçues
        size_t taille_donnees = n - 4;
        buffer_final = realloc(buffer_final, taille_totale + taille_donnees);
        memcpy(buffer_final + taille_totale, buffer_reception + 4, taille_donnees);
        taille_totale += taille_donnees;

        // Préparation et envoi du nouvel ACK
        dernier_block_recu = ntohs(*(uint16_t *)(buffer_reception + 2));
        memcpy(ack + 2, buffer_reception + 2, 2);
        if (peer_set) {
            if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len) < 0) perror("sendto");
        } else {
            if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) perror("sendto");
        }
        printf("  [PUT] ACK %d envoyé\n", dernier_block_recu);

    } while (n == 516);

    if (!recu_ok) {
        free(buffer_final);
        if (mtx) pthread_mutex_unlock(&mtx->mutex);
        close(sockfd);
        return;
    }

    if (strstr(filename, "..")) {
        printf("  [SERVER] Erreur : Tentative d'accès non autorisé '%s'.\n", filename);
        free(buffer_final);
        if (mtx) pthread_mutex_unlock(&mtx->mutex);
        close(sockfd);
        return;
    }

    mkdir(REPOSITORY, 0777);

    char chemin[256];
    snprintf(chemin, sizeof(chemin), REPOSITORY "%s", filename);
    FILE *f = fopen(chemin, "wb");
    if (f) { 
        fwrite(buffer_final, 1, taille_totale, f); 
        printf("  [PUT] Transfert de '%s' terminé.\n", filename);
        fclose(f); 
    } else {
        printf("  [SERVER] Erreur : Impossible d'écrire le fichier '%s'.\n", chemin);
    }
    free(buffer_final);
    if (mtx) pthread_mutex_unlock(&mtx->mutex);
    close(sockfd);
}

int main() {
    int server_fd;
    struct sockaddr_in server_addr, client_addr;
    char buffer[MAX_BUF];
    socklen_t addr_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }

    printf("[SERVER] En attente sur le port %d...\n", PORT);
    while (1) {
        ssize_t n = recvfrom(server_fd, buffer, MAX_BUF, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 4) continue;
        uint16_t opcode = ntohs(*(uint16_t *)buffer);
        pthread_t tid;
        if (opcode == 1 || opcode == 2) {
            void* params = malloc(sizeof(struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[516];}));
            memcpy(&((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[516];}*)params)->client_addr, &client_addr, sizeof(client_addr));
            ((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[516];}*)params)->addr_len = addr_len;
            memcpy(((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[516];}*)params)->fichier, buffer + 2, 516);
            if (opcode == 1)
                pthread_create(&tid, NULL, thread_rrq, params);
            else
                pthread_create(&tid, NULL, thread_wrq, params);
            pthread_detach(tid);
        }
    }
    return 0;
}