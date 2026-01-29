#include "../include/common.h"

void handle_read_request(struct sockaddr_in client_addr, const char *filename);

int main() {
    int main_socket;
    struct sockaddr_in server_addr, client_addr;
    uint8_t buffer[MAX_PACKET_SIZE];
    socklen_t addr_len = sizeof(client_addr);

    //  Initialiser le socket UDP
    if ((main_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Main socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TFTP_PORT);

    //  Connexion au port 69 (Nécessite les droits sudo)
    if (bind(main_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("La liaison a échoué. Essayez d'executer le programme avec sudo");
        close(main_socket);
        exit(EXIT_FAILURE);
    }

    printf("[SERVEUR] Serveur TFTP à l'écoute sur le port %d...\n", TFTP_PORT);

    while (1) {
        ssize_t received = recvfrom(
            main_socket, 
            buffer,
            MAX_PACKET_SIZE, 
            0, 
            (struct sockaddr *)&client_addr, &addr_len
        );
        
        if (received < 4) continue;

        uint16_t opcode;
        memcpy(&opcode, buffer, 2);
        opcode = ntohs(opcode);

        // Safe filename extraction (standard TFTP format: opcode + filename + 0 + mode + 0)
        char *filename = (char *)&buffer[2];

        if (opcode == OP_RRQ) {
            handle_read_request(client_addr, filename);
        } else if (opcode == OP_WRQ) {
            printf("[SERVER] Write request for: %s (Not yet implemented)\n", filename);
        }
    }

    close(main_socket);
    return 0;
}

/**
 * Handles the actual data transfer on a separate socket (TID)
 * to free up port 69 for new incoming requests.
 */
void handle_read_request(struct sockaddr_in client_addr, const char *filename) {
    int tid_socket;
    FILE *file;
    uint8_t buffer[MAX_PACKET_SIZE];
    uint16_t block_num = 1;
    ssize_t read_bytes;

    // 1. Open the file in binary mode
    file = fopen(filename, "rb");
    if (!file) {
        perror("[ERROR] Could not open file");
        // Future step: Send OP_ERROR packet to client
        return;
    }

    // 2. Create TID socket (Ephemeral port)
    if ((tid_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("[ERROR] Failed to create TID socket");
        fclose(file);
        return;
    }

    printf("[TID] Starting transfer: %s to %s:%d\n", 
        filename, 
        inet_ntoa(client_addr.sin_addr), 
        ntohs(client_addr.sin_port)
    );

    // 3. Transfer Loop (Stop-and-Wait)
    while ((read_bytes = fread(buffer + 4, 1, MAX_TFTP_PAYLOAD, file)) > 0 || block_num==1) {
        // Build DATA packet header (Opcode 3 + Block #)
        uint16_t opcode = htons(OP_DATA);
        uint16_t block_net = htons(block_num);

        memcpy(buffer, &opcode, 2);
        memcpy(buffer + 2, &block_net, 2);


        if(sendto(tid_socket, 
            buffer, 
            read_bytes+4,
            0, 
            (struct sockaddr *)&client_addr, 
            sizeof(client_addr))<0
        ){
            perror("[ERROR] sento failed");
            break;
        }
        // Send packet

        //sendto(tid_socket, packet, read_len + 4, 0, 
        //       (struct sockaddr *)&client_addr, sizeof(client_addr));
        
        printf("[DATA] Block %d sent (%zd bytes)\n", block_num, read_bytes);

        /* * Professional Note: In a full implementation, you must wait for 
         * an OP_ACK with the matching block_num here before continuing.
         */

        if (read_bytes < MAX_TFTP_PAYLOAD) break; // Last packet sent
        block_num++;
    }

    printf("[INFO] Transfer of %s finished.\n", filename);
    fclose(file);
    close(tid_socket);
}