#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define TFTP_PORT 69
#define MAX_TFTP_PAYLOAD 512
#define MAX_PACKET_SIZE 516 // 2 bytes Opcode + 2 bytes Block/Error + 512 Data

// TFTP Opcodes as defined in RFC 1350
typedef enum {
    OP_RRQ   = 1,
    OP_WRQ   = 2,
    OP_DATA  = 3,
    OP_ACK   = 4,
    OP_ERROR = 5
} tftp_opcode_t;

// Use packed attribute to prevent compiler padding
// ensuring the struct matches the RFC wire format exactly.
struct __attribute__((packed)) tftp_data_packet {
    uint16_t opcode;
    uint16_t block_num;
    char data[MAX_TFTP_PAYLOAD];
};

struct __attribute__((packed)) tftp_ack_packet {
    uint16_t opcode;
    uint16_t block_num;
};

#endif