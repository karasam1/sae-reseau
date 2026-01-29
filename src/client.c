#include "../include/common.h"

/**
 * @brief Encapsulates the TFTP client session state.
 */
typedef struct {
    int sockfd;
    struct sockaddr_in server_addr;
    const char *filename;
    const char *mode;
} tftp_session_t;

ssize_t tftp_send_request(tftp_session_t *session, tftp_opcode_t opcode);

int main(int argc, char const *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <get|put> <filename> [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    tftp_session_t session;
    session.filename = argv[3];
    session.mode = "octet"; // Binary transfer mode as per RFC 1350
    int port = (argc == 5) ? atoi(argv[4]) : TFTP_PORT;

    // Initialize Network Socket
    if ((session.sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }

    memset(&session.server_addr, 0, sizeof(session.server_addr));
    session.server_addr.sin_family = AF_INET;
    session.server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, argv[1], &session.server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP address\n");
        close(session.sockfd);
        return EXIT_FAILURE;
    }

    // Execute Request
    tftp_opcode_t opcode = (strcmp(argv[2], "get") == 0) ? OP_RRQ : OP_WRQ;
    
    if (tftp_send_request(&session, opcode) < 0) {
        fprintf(stderr, "Failed to send initial request\n");
        close(session.sockfd);
        return EXIT_FAILURE;
    }

    printf("[CLIENT] Request (%s) sent to %s:%d\n", argv[2], argv[1], port);
    printf("[INFO] Handshake initiated. Awaiting TID from server...\n");

    /* * RATIONALE: According to RFC 1350, the client must now enter a loop 
     * to receive the first DATA (for GET) or ACK (for PUT) packet. 
     * The source port of that packet will be used as the TID for the rest 
     * of the transfer.
     */

    close(session.sockfd);
    return EXIT_SUCCESS;
}

ssize_t tftp_send_request(tftp_session_t *session, tftp_opcode_t opcode) {
    uint8_t packet[MAX_PACKET_SIZE];
    uint16_t net_opcode = htons(opcode);
    size_t offset = 0;

    // 1. Header: Opcode (2 bytes)
    memcpy(packet + offset, &net_opcode, sizeof(uint16_t));
    offset += sizeof(uint16_t);

    // 2. Body: Filename + Null terminator
    size_t file_len = strlen(session->filename);
    if (offset + file_len + 1 > MAX_PACKET_SIZE) return -1;
    
    memcpy(packet + offset, session->filename, file_len + 1);
    offset += file_len + 1;

    // 3. Body: Mode + Null terminator
    size_t mode_len = strlen(session->mode);
    if (offset + mode_len + 1 > MAX_PACKET_SIZE) return -1;

    memcpy(packet + offset, session->mode, mode_len + 1);
    offset += mode_len + 1;

    return sendto(session->sockfd, 
        packet, 
        offset, 
        0, 
        (struct sockaddr *)&session->server_addr, 
        sizeof(session->server_addr)
    );
}