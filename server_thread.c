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
#define REPOSITORY ".tftp/"
#define PORT 69
#define MAX_BUF 516
#define TFTP_TIMEOUT_SEC 5
#define TFTP_MAX_ESSAI 5

typedef struct {
    char filename[256];
    pthread_mutex_t mutex;
    bool in_use;
} file_mutex_t;

file_mutex_t file_mutexes[MAX_FILES];
pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;

// Get or create a mutex for a specific file
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
    return NULL; // Should ideally handle this case better (e.g. increase MAX_FILES or fail gracefully)
}

void send_error(int sockfd, struct sockaddr_in *client_addr, socklen_t addr_len, uint16_t err_code, const char *err_msg) {
    char err_packet[MAX_BUF];
    uint16_t opcode = htons(5); 
    uint16_t error_code = htons(err_code); 
    memcpy(err_packet, &opcode, 2);
    memcpy(err_packet + 2, &error_code, 2);
    int len = 4 + sprintf(err_packet + 4, "%s", err_msg) + 1;
    sendto(sockfd, err_packet, len, 0, (struct sockaddr *)client_addr, addr_len);
}

void traitement_rrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier);
void traitement_wrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier);

void* thread_rrq(void* arg) {
    struct {
        struct sockaddr_in client_addr;
        socklen_t addr_len;
        char fichier[MAX_BUF];
    } *params = arg;
    traitement_rrq(&params->client_addr, params->addr_len, params->fichier);
    free(params);
    return NULL;
}

void* thread_wrq(void* arg) {
    struct {
        struct sockaddr_in client_addr;
        socklen_t addr_len;
        char fichier[MAX_BUF];
    } *params = arg;
    traitement_wrq(&params->client_addr, params->addr_len, params->fichier);
    free(params);
    return NULL;
}

void traitement_rrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0); 
    
    if (sockfd < 0) {
        perror("socket");
        return;
    }
    
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0}; 
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *filename = fichier;
    const char *mode = fichier + strlen(filename) + 1;

    // Vérifications de base des noms de fichiers
    if (strstr(filename, "..")) {
        send_error(sockfd, client_addr, addr_len, 2, "Violation d'accès");
        close(sockfd);
        return;
    }

    char chemin[256];
    snprintf(chemin, sizeof(chemin), REPOSITORY "%s", filename);
    
    file_mutex_t* mtx = get_file_mutex(filename);
    if (mtx) pthread_mutex_lock(&mtx->mutex);
    
    FILE *f = fopen(chemin, "rb");

    if (!f) {
        if (mtx) pthread_mutex_unlock(&mtx->mutex);
        send_error(sockfd, client_addr, addr_len, 1, "Fichier non trouvé");
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

        while (tentatives < TFTP_MAX_ESSAI && !ack_recu) {
            if (sendto(sockfd, buffer, read_len + 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) {
                perror("sendto");
                break;
            }
            
            ssize_t r = recvfrom(sockfd, ack_buf, 4, 0, (struct sockaddr *)&peer_addr, &peer_len);
            if (r >= 4) {
                if (peer_addr.sin_addr.s_addr != client_addr->sin_addr.s_addr || peer_addr.sin_port != client_addr->sin_port) {
                    send_error(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                    continue; 
                }
                uint16_t op = ntohs(*(uint16_t *)ack_buf);
                uint16_t ack_val = ntohs(*(uint16_t *)(ack_buf + 2));
                if (op == 4 && ack_val == block_num) {
                    ack_recu = 1;
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
            } else {
                break;
            }
        }

        if (!ack_recu) break;
        block_num++;
    } while (read_len == 512);

    printf("[THREAD] Download '%s' finished.\n", filename);
    fclose(f);
    if (mtx) pthread_mutex_unlock(&mtx->mutex);
    close(sockfd);
}

void traitement_wrq(struct sockaddr_in *client_addr, socklen_t addr_len, const char *fichier) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }

    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const char *filename = fichier;
    
    if (strstr(filename, "..")) {
        send_error(sockfd, client_addr, addr_len, 2, "Access violation");
        close(sockfd);
        return;
    }

    file_mutex_t* mtx = get_file_mutex(filename);
    if (mtx) pthread_mutex_lock(&mtx->mutex);

    // Initial ACK 0
    char ack[4] = {0, 4, 0, 0}; // Opcode 4 (ACK), Block 0
    if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)client_addr, addr_len) < 0) perror("sendto");

    char buffer_reception[MAX_BUF];
    char *buffer_final = NULL;
    size_t taille_totale = 0;
    uint16_t dernier_block_recu = 0;
    ssize_t n;
    
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int peer_set = 0;
    int recu_ok = 0;

    do {
        int tentatives = 0;

        while (tentatives < TFTP_MAX_ESSAI && !recu_ok) {
            ssize_t r = recvfrom(sockfd, buffer_reception, MAX_BUF, 0, (struct sockaddr *)&peer_addr, &peer_len);

            if (r >= 4) {
                if (!peer_set) peer_set = 1;
                
                if (peer_addr.sin_addr.s_addr != client_addr->sin_addr.s_addr || peer_addr.sin_port != client_addr->sin_port) {
                    send_error(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                    continue;
                }

                uint16_t op = ntohs(*(uint16_t *)buffer_reception);
                uint16_t block_recu = ntohs(*(uint16_t *)(buffer_reception + 2));

                if (op == 3) { // DATA
                    if (block_recu == dernier_block_recu + 1) {
                        recu_ok = 1;
                        n = r;
                    } else if (block_recu == dernier_block_recu) {
                        // Resend ACK for duplicate data
                        uint16_t ack_op = htons(4);
                        uint16_t ack_blk = htons(dernier_block_recu);
                        char re_ack[4];
                        memcpy(re_ack, &ack_op, 2);
                        memcpy(re_ack + 2, &ack_blk, 2);
                        sendto(sockfd, re_ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len);
                    }
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
                // Resend last ACK on timeout
                uint16_t ack_op = htons(4);
                uint16_t ack_blk = htons(dernier_block_recu);
                char re_ack[4];
                memcpy(re_ack, &ack_op, 2);
                memcpy(re_ack + 2, &ack_blk, 2);
                sendto(sockfd, re_ack, 4, 0, (struct sockaddr *)client_addr, addr_len);
            } else {
                break;
            }
        }

        if (!recu_ok) break;

        size_t taille_donnees = n - 4;
        buffer_final = realloc(buffer_final, taille_totale + taille_donnees);
        memcpy(buffer_final + taille_totale, buffer_reception + 4, taille_donnees);
        taille_totale += taille_donnees;

        dernier_block_recu++;
        
        uint16_t ack_op = htons(4);
        uint16_t ack_blk = htons(dernier_block_recu);
        char new_ack[4];
        memcpy(new_ack, &ack_op, 2);
        memcpy(new_ack + 2, &ack_blk, 2);
        sendto(sockfd, new_ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len);

    } while (n == MAX_BUF);

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
        fclose(f);
        printf("[THREAD] Upload '%s' finished.\n", filename);
    } else {
        perror("fopen");
        send_error(sockfd, client_addr, addr_len, 2, "Access violation");
    }

    free(buffer_final);
    if (mtx) pthread_mutex_unlock(&mtx->mutex);
    close(sockfd);
}

int main() {
    int server_fd;
    
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;

    char buffer[MAX_BUF];
    socklen_t addr_len = sizeof(client_addr);

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        perror("Socket error");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        return 1;
    }

    printf("[SERVER-THREAD] Waiting on port %d...\n", PORT);
    while (1) {
        ssize_t n = recvfrom(server_fd, buffer, MAX_BUF, 0, (struct sockaddr *)&client_addr, &addr_len);
        if (n < 4) continue;
        
        uint16_t opcode = ntohs(*(uint16_t *)buffer);   //  (nhtons : Network to Host Short)
                                                        //  16 bits, convertit de l'ordre réseau (big-endian) à l'ordre hôte (endianness de la machine)       
        pthread_t tid;
        
        if (opcode == 1 || opcode == 2) {
            //  Valider la structure du paquet: Opcode | Filename | 0 | Mode | 0
            //  Le nom du fichier et le MODE se terminent par un caractère nul dans le tampon
            char *filename = buffer + 2;
            char *mode = NULL;
            char *end = buffer + n;
            
            //  Vérifier la présence d'un caractère nul de fin de nom du fichier
            char *p = filename;
            while (p < end && *p) p++;
            if (p >= end - 1) { 
                //  MODE mal formaté ou manquant
                const char *err = "Malformed packet";
                send_error(server_fd, &client_addr, addr_len, 4, err); // 4 = Illegal TFTP operation
                continue; 
            }
            
            mode = p + 1;
            //  Vérifier le terminateur nul du MODE
            p = mode;
            while (p < end && *p) p++;
            if (p >= end) {
                 const char *err = "Malformed packet";
                 send_error(server_fd, &client_addr, addr_len, 4, err);
                 continue;
            }
            
            // Validate Mode "octet" (case-insensitive)
            if (strcasecmp(mode, "octet") != 0) {
                 const char *err = "Only octet mode supported";
                 send_error(server_fd, &client_addr, addr_len, 4, err); // 4 = Illegal TFTP
                 continue;
            }

            // Allouer de la memoire pour les arguments du thread
            void* params = malloc(sizeof(
                struct {
                    struct sockaddr_in client_addr; 
                    socklen_t addr_len; 
                    char fichier[MAX_BUF];
                }
            ));
            if (!params) {
                perror("malloc");
                continue; 
            }
            
            memcpy(&((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[MAX_BUF];}*)params)->client_addr, &client_addr, sizeof(client_addr));
            ((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[MAX_BUF];}*)params)->addr_len = addr_len;
            
            // Copie sécurisée de FILENAME + 0 + MODE + 0 dans le tampon (max MAX_BUF)
            // We already validated format, but let's be safe.
            // The logic in thread function uses: filename = fichier; mode = fichier + strlen + 1;
            // So copying the raw buffer from offset 2 is fine, as validated above.
            size_t data_len = (p + 1) - (buffer + 2);
            if (data_len > MAX_BUF) data_len = MAX_BUF; // Should not happen given MAX_BUF=MAX_BUF and headers
            
            memset(((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[MAX_BUF];}*)params)->fichier, 0, MAX_BUF);
            memcpy(((struct {struct sockaddr_in client_addr; socklen_t addr_len; char fichier[MAX_BUF];}*)params)->fichier, buffer + 2, data_len);

            if (opcode == 1)
                pthread_create(&tid, NULL, thread_rrq, params);
            else
                pthread_create(&tid, NULL, thread_wrq, params);
            
            pthread_detach(tid);
        }
    }
    return 0;
}
