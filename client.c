#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 69
#define MAX_BUF 516 // 512 octets de données + 4 octets d'en-tête

int main() {
    int client_fd;
    struct sockaddr_in serv_addr;
    char buffer[MAX_BUF];
    
    // 1. Changement SOCK_STREAM -> SOCK_DGRAM (UDP) car TFTP utilise UDP et non TCP (etudier les différences entre les 2 protocoles)
    if ((client_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    // 2. Construction d'un paquet RRQ (Read Request)
    // Format : Opcode (2 octets) + Fichier + 0 + Mode + 0
    short opcode = htons(1); // 1 = RRQ
    char *filename = "test.txt";
    char *mode = "octet";
    
    int pos = 0;
    memcpy(buffer + pos, &opcode, 2); pos += 2;
    strcpy(buffer + pos, filename); pos += strlen(filename) + 1;
    strcpy(buffer + pos, mode); pos += strlen(mode) + 1;

    // 3. Envoi avec sendto (car UDP)
    sendto(client_fd, buffer, pos, 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    printf("Requête RRQ envoyée pour le fichier : %s\n", filename);

    // 4. Réception de la réponse
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    int n = recvfrom(client_fd, buffer, MAX_BUF, 0, (struct sockaddr*)&from_addr, &from_len);

    if (n > 0) {
        short resp_opcode = ntohs(*(short*)buffer);
        if (resp_opcode == 3) { // 3 = DATA
            printf("Données reçues du serveur (depuis le port %d)\n", ntohs(from_addr.sin_port));
        } else if (resp_opcode == 5) { // 5 = ERROR
            printf("Erreur reçue du serveur\n");
        }
    }

    close(client_fd);
    return 0;
}