#ifndef TFTP_PROTOCOLE_H
#define TFTP_PROTOCOLE_H

#include <stdint.h>
#include <netinet/in.h>
#include <stdio.h>

// --- CONSTANTES DU PROTOCOLE (RFC 1350) ---
#define TFTP_PORT           69
#define MAX_PACKET_SIZE     516    // 2 octets Opcode + 2 octets Block + 512 octets Data
#define MAX_DATA_SIZE       512
#define TFTP_TIMEOUT_SEC    5      // Délai avant retransmission
#define TFTP_MAX_RETRIES    5      // Nombre max de tentatives

// --- CODES D'OPÉRATION (OPCODES) ---
#define TFTP_OP_RRQ   1  // Read Request
#define TFTP_OP_WRQ   2  // Write Request
#define TFTP_OP_DATA  3  // Data packet
#define TFTP_OP_ACK   4  // Acknowledgment
#define TFTP_OP_ERR   5  // Error packet

// --- STRUCTURES DES PAQUETS ---

// Paquet DATA
typedef struct {
    uint16_t opcode;
    uint16_t block;
    uint8_t  data[MAX_DATA_SIZE];
} __attribute__((packed)) tftp_data_packet_t;

// Paquet ACK
typedef struct {
    uint16_t opcode;
    uint16_t block;
} __attribute__((packed)) tftp_ack_packet_t;

// Paquet ERROR
typedef struct {
    uint16_t opcode;
    uint16_t err_code;
    char     message[512];
} __attribute__((packed)) tftp_error_packet_t;

// --- CONTEXTE DE TRANSFERT ---
// Utilisé par les threads du serveur et par le client pour suivre l'état
typedef struct {
    int                sockfd;       // Socket dédiée au transfert (TID)
    struct sockaddr_in peer_addr;    // Adresse du correspondant (Client ou Serveur)
    socklen_t          peer_len;
    char               filename[256];
    char               mode[32];     // Généralement "octet"
    FILE               *fp;          // Descripteur du fichier local
    uint16_t           last_block;   // Numéro du dernier bloc acquitté
    int                retries;      // Compteur de retransmissions
    int                est_ecrivain; // 1 pour WRQ, 0 pour RRQ
} tftp_context_t;

#endif // TFTP_PROTOCOLE_H