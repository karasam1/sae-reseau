#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 69 // Attention: nécessite sudo pour bind le port 69

int main() {
    int server_fd;
    struct sockaddr_in address, client_addr;
    socklen_t addrlen = sizeof(client_addr);
    char buffer[516];

    // 1. UDP Socket
    if ((server_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed (utilisez sudo pour le port 69)");
        exit(EXIT_FAILURE);
    }

    printf("Serveur TFTP en attente sur le port %d...\n", PORT);

    // 2. Lecture du paquet entrant
    int n = recvfrom(server_fd, buffer, 516, 0, (struct sockaddr*)&client_addr, &addrlen);
    
    if (n > 0) {
        short opcode = ntohs(*(short*)buffer);
        printf("Requête reçue ! Opcode: %d\n", opcode);
        
        if (opcode == 1) { // RRQ
            char *filename = buffer + 2;
            printf("Le client veut lire le fichier : %s\n", filename);
            
            // Ici, normalement on devrait créer un THREAD pour gérer 
            // l'envoi du fichier sur un nouveau port.
        }
    }

    close(server_fd);
    return 0;
}