#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>

#define PORT 69
#define MAX_BUF 516
#define TFTP_TIMEOUT_SEC 5
#define TFTP_MAX_RETRIES 5

typedef struct { 
    const char *ip;
    int port; 
    const char *fichier; 
    int type ; // 1 pour get , 2 pour put
} requete_tftp_t;

void send_request(int sockfd, struct sockaddr_in *server_addr, uint16_t opcode_val, const char *fichier) {
    char buffer[MAX_BUF];
    memset(buffer, 0, MAX_BUF); // Limpiamos el buffer por seguridad
    
    int idx = 0;

    // 1. Opcode (2 bytes) - Usando htons para Network Byte Order
    uint16_t opcode = htons(opcode_val);
    memcpy(buffer + idx, &opcode, 2);
    idx += 2;

    // 2. Filename (N bytes)
    size_t fn_len = strlen(fichier);
    memcpy(buffer + idx, fichier, fn_len);
    idx += fn_len;
    
    // 3. Primer delimitador nulo (1 byte)
    buffer[idx++] = '\0';

    // 4. Mode (octet)
    const char *mode = "octet";
    size_t mode_len = strlen(mode);
    memcpy(buffer + idx, mode, mode_len);
    idx += mode_len;

    // 5. Segundo delimitador nulo (1 byte)
    buffer[idx++] = '\0';

    // Enviamos exactamente 'idx' bytes
    if (sendto(sockfd, buffer, idx, 0, (struct sockaddr *)server_addr, (socklen_t)sizeof(*server_addr)) < 0) {
        perror("[ERROR] send_request failed");
    }
}

 void send_error_client(int sockfd, struct sockaddr_in *peer, socklen_t peer_len, uint16_t err_code, const char *err_msg) {
    char err_packet[MAX_BUF];
    uint16_t opcode = htons(5);
    uint16_t error_code = htons(err_code);
    memcpy(err_packet, &opcode, 2);
    memcpy(err_packet + 2, &error_code, 2);
    int len = 4 + sprintf(err_packet + 4, "%s", err_msg) + 1;
    sendto(sockfd, err_packet, len, 0, (struct sockaddr *)peer, peer_len);
}

int get(int sockfd, struct sockaddr_in *server_addr, const char *fichier) {
    // Configurer le timeout sur la socket
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    
    // Appel à une fonction système (System Call) pour configurer le timeout de réception sur la socket.
    //  sockfd : Identificateur du socket du client
    //  SOL_SOCKET : Cela indique que l'option à modifier se situe au niveau général du <<socket>>; cela ne spécifie pas de protocole TCP or UDP.
    //  SO_RCVTIMEO :   Option de socket pour définir le délai d'attente pour les opérations de réception (recvfrom).
    //                  Cela signifie (Receive Timeout - Delai d'attente de réception)
    //  &tv : Pointeur vers la structure timeval
    //  
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char *buffer_final = NULL;
    size_t taille_totale = 0;
    char buffer[MAX_BUF];
    ssize_t n;
    socklen_t addr_len = sizeof(*server_addr);
    int is_valid = 1;
    uint16_t dernier_lock_recu = 0; // Pour savoir quel ACK renvoyer
    uint16_t server_tid = 0; // le port TID du serveur une fois connu

    printf("[GET] Téléchargement de '%s'...\n", fichier);

    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    int peer_set = 0;
    memset(&peer_addr, 0, sizeof(peer_addr));

    do {
        int tentatives = 0;
        int recu_ok = 0;

        while (tentatives < TFTP_MAX_RETRIES && !recu_ok) {
            //  recvfrom : Fonction système pour recevoir des données sur une socket UDP
            //  Si le serveur répond, on reçoit un paquet et on vérifie son contenu.
            n = recvfrom(sockfd, buffer, MAX_BUF, 0, (struct sockaddr *)&peer_addr, &peer_len);

            if (n >= 4) {
                if (!peer_set) {
                    if (peer_addr.sin_addr.s_addr != server_addr->sin_addr.s_addr) {
                        printf("[WARNING] Reçu paquet depuis %s (attendu %s) -> envoi ERROR(5)\n",
                            inet_ntoa(peer_addr.sin_addr), inet_ntoa(server_addr->sin_addr));
                        send_error_client(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                        continue;
                    }
                    server_tid = peer_addr.sin_port;
                    peer_set = 1;
                    printf("[GET] Serveur TID identifié: %d\n", ntohs(server_tid));
                    recu_ok = 1;
                } else {
                    if (peer_addr.sin_port != server_tid) {
                        printf("[WARNING] Paquet depuis TID incorrect -> envoi ERROR(5)\n");
                        send_error_client(sockfd, &peer_addr, peer_len, 5, "Unknown transfer ID");
                        continue;
                    }
                    recu_ok = 1;
                }
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
                printf("[TIMEOUT] Tentative %d/%d ... Renvoi du dernier message.\n", tentatives, TFTP_MAX_RETRIES);
                if (dernier_lock_recu == 0) {
                    send_request(sockfd, server_addr, 1, fichier); // Operation Code 1 = RRQ (Read Request)
                } else {
                    if (peer_set) {
                        uint16_t blk_net = htons(dernier_lock_recu);
                        char ack_retry[4] = {0, 4, ((char*)&blk_net)[0], ((char*)&blk_net)[1]};
                        peer_addr.sin_port = server_tid;
                        if (sendto(sockfd, ack_retry, 4, 0, (struct sockaddr *)& peer_addr, peer_len) < 0) {
                            perror("sendto");
                            break;
                        }
                    } else {
                        send_request(sockfd, server_addr, 1, fichier);
                    }
                }
            } else {
                if (n < 0) perror("recvfrom");
                is_valid = 0;
                break;
            }
        }

        if (!recu_ok) { is_valid = 0; break; } // Abandon après 5 échecs

        // VERIFICATION SI C'EST UN PAQUET D'ERREUR (Opcode 5)
        uint16_t opcode = ntohs(*(uint16_t *)buffer);
        if (opcode == 5) { is_valid = 0; break; }

        size_t data_len = n - 4;
        uint16_t block_num = ntohs(*(uint16_t *)(buffer + 2));

        if (block_num == dernier_lock_recu + 1) {
            buffer_final = realloc(buffer_final, taille_totale + data_len);
            memcpy(buffer_final + taille_totale, buffer + 4, data_len);
            taille_totale += data_len;
            dernier_lock_recu = block_num; // On mémorise le nouveau bloc
        } else if (block_num == dernier_lock_recu) {
            printf("[GET] Doublon reçu (bloc %d), renvoi de l'ACK sans écriture.\n", block_num);
        }
        // Si c'est un autre bloc (avance rapide ou vieux), on ignore ou on ack le dernier reçu. 
        // Ici on va juste ACK le block_num reçu si c'est le doublon ou le nouveau.
        
        char ack[4] = {0, 4, buffer[2], buffer[3]};
        if (peer_set) {
            peer_addr.sin_port = server_tid;
            if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)&peer_addr, peer_len) < 0) perror("sendto");
        } else {
            if (sendto(sockfd, ack, 4, 0, (struct sockaddr *)server_addr, addr_len) < 0) perror("sendto");
        }
        printf("[GET] ACK %d envoyé\n", block_num);

    } while (n == 516);

    if (is_valid) {
        FILE *f = fopen(fichier, "wb");
        if (f) {
            fwrite(buffer_final, 1, taille_totale, f);
            fclose(f);
            printf("[GET] Fichier '%s' reçu et formé (%zu octets).\n", fichier, taille_totale);
        }
    } else {
        // Message d'erreur si is_valid est passé à 0
        printf("[GET] ERREUR : Le transfert a échoué (erreur serveur).\n");
    }
    free(buffer_final);
    return 0;
}

int put(int sockfd, struct sockaddr_in *server_addr, const char *fichier) {
    struct timeval tv = {TFTP_TIMEOUT_SEC, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    FILE *f = fopen(fichier, "rb");
    if (!f) {
        fprintf(stderr, "[PUT] ERREUR : Le fichier '%s' n'existe pas.\n", fichier);
        return -1; 
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *full_data = malloc(fsize);
    fread(full_data, 1, fsize, f);
    fclose(f);

    // Phase 1 : Envoi WRQ et attente ACK 0 (avec timeout/retries)
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    char buffer[MAX_BUF];
    int tentatives = 0;
    int recu_ok = 0;
    memset(&peer_addr, 0, sizeof(peer_addr));
    int peer_set = 0;

    while (tentatives < TFTP_MAX_RETRIES && !recu_ok) {
        send_request(sockfd, server_addr, 2, fichier);
        ssize_t r = recvfrom(sockfd, buffer, MAX_BUF, 0, (struct sockaddr *)&peer_addr, &peer_len);
        if (r >= 4) {
            if (ntohs(*(uint16_t *)buffer) == 4 && ntohs(*(uint16_t *)(buffer + 2)) == 0) {
                recu_ok = 1;
                peer_set = 1;
                printf("[PUT] ACK 0 reçu, TID: %d\n", ntohs(peer_addr.sin_port));
            }
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            tentatives++;
            printf("[TIMEOUT] Tentative %d/%d... Renvoi de la requête WRQ.\n", tentatives, TFTP_MAX_RETRIES);
        } else {
            if (r < 0) perror("recvfrom");
            break;
        }
    }
    if (!recu_ok) { free(full_data); return -1; }

    // peer_addr already contains the correct TID from ACK 0 response - do NOT overwrite


    // Phase 2 : Envoi des blocs DATA avec timeout/retries
    size_t sent = 0;
    uint16_t block = 1;
    do {
        // 1. Preparación del tamaño y limpieza
        size_t to_send = (fsize - sent > 512) ? 512 : (fsize - sent);
        memset(buffer, 0, MAX_BUF); // Evitar basura de bloques anteriores

        // 2. Construcción del encabezado DATA (Opcode 3)
        uint16_t op = htons(3);
        uint16_t blk = htons(block);
        memcpy(buffer, &op, 2);
        memcpy(buffer + 2, &blk, 2);
        
        // Copiamos los datos del archivo solo si hay algo que enviar
        if (to_send > 0) {
            memcpy(buffer + 4, full_data + sent, to_send);
        }

        tentatives = 0;
        recu_ok = 0;
        
        while (tentatives < TFTP_MAX_RETRIES && !recu_ok) {
            if (sendto(sockfd, buffer, to_send + 4, 0, (struct sockaddr *)&peer_addr, peer_len) < 0) {
                perror("sendto");
                break;
            }
            printf("[PUT] Envoi du bloc %d (%zu octets)...\n", block, to_send);

            struct sockaddr_in ack_addr;
            socklen_t ack_len = sizeof(ack_addr);
            
            ssize_t r = recvfrom(sockfd, buffer, MAX_BUF, 0, (struct sockaddr *)&ack_addr, &ack_len);
            
            if (r >= 4) {
                uint16_t received_opcode = ntohs(*(uint16_t *)buffer);
                uint16_t received_block = ntohs(*(uint16_t *)(buffer + 2));

                if (received_opcode == 5) {
                    printf("[PUT] ERROR SERVEUR: %s\n", buffer + 4);
                    free(full_data);
                    return -1; 
                }

                if (received_opcode == 4) {
                    if (ack_addr.sin_port == peer_addr.sin_port) {
                        if (received_block == block) {
                            recu_ok = 1;
                            printf("[PUT] ACK %d reçu\n", received_block);
                        }
                    } else {
                        send_error_client(sockfd, &ack_addr, ack_len, 5, "Unknown transfer ID");
                    }
                }
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                tentatives++;
                printf("[TIMEOUT] Bloc %d non acquitté, tentative %d/%d...\n", block, tentatives, TFTP_MAX_RETRIES);
            } else {
                if (r < 0) perror("recvfrom");
                break;
            }
        }

        if (!recu_ok) break;

        sent += to_send;
        block++;

        // La condición de salida: si enviamos un bloque de menos de 512, termina.
        // Si el archivo es múltiplo de 512, se enviará un último paquete con to_send = 0.
    } while (sent < fsize || (sent == fsize && (fsize % 512 == 0) && fsize > 0));
    
    printf("[PUT] Envoi de '%s' terminé.\n", fichier);
    free(full_data);
    return 0;
}

int main(int argc, char const *argv[]) {
    if (argc < 4 || argc > 5) {
        printf("Usage: %s <ip> <get|put> <fichier> [port]\n", argv[0]);
        return 1;
    }
    
    // User requested syntax: ./client <ip> <get|put> <file> <port>
    // Argv mapping:
    //   argv[1]: ip
    //   argv[2]: get|put
    //   argv[3]: fichier
    //   argv[4]: port (optional) (default 69)

    const char *ip = argv[1];
    const char *cmd = argv[2];
    const char *filename = argv[3];
    int port = PORT;
    
    if (argc == 5) {
        port = atoi(argv[4]);
    }
    
    int type = 0;
    if (strcasecmp(cmd, "get") == 0) type = 1;
    else if (strcasecmp(cmd, "put") == 0) type = 2;
    else {
        printf("Erreur: Commande invalide '%s'. Utiliser 'get' ou 'put'.\n", cmd);
        return 1;
    }

    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;

    // htons : Host To Network Short
    // Convertit le port de l'ordre d'octets de l'hôte à l'ordre d'octects de réseau (big-endian).
    server_addr.sin_port = htons(port); 

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        printf("Erreur: Adresse IP invalide '%s'\n", ip);
        close(client_fd);
        return 1;
    }

    if (type == 1) 
        get(client_fd, &server_addr, filename);
    else 
        put(client_fd, &server_addr, filename);

    close(client_fd);
    return 0;
}