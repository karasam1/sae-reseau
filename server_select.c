#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdbool.h>

#define PORT 69
#define REPOSITORY ".tftp/"
#define MAX_BUF 516
#define MAX_CLIENTS 10
#define TFTP_TIMEOUT_SEC 5
#define MAX_RETRIES 5
#define MAX_FILES 128

// --- Structures ---

typedef enum {
    STATE_NONE,
    STATE_RRQ, // Server sending data
    STATE_WRQ  // Server receiving data
} ClientState;

typedef struct {
    int sockfd;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    
    ClientState state;
    char filename[256];
    FILE *fp;
    
    uint16_t block_num;      // Next block to send (RRQ) or expected block (WRQ)
    char buffer[MAX_BUF];    // Data buffer
    int buffer_len;
    
    time_t last_activity;
    int retries;
    
    bool active;
} ClientContext;

typedef struct {
    char filename[256];
    bool in_use;
} FileLock;

FileLock file_locks[MAX_FILES];
ClientContext clients[MAX_CLIENTS];

// --- Helpers ---

void init_globals() {
    for (int i = 0; i < MAX_FILES; i++) file_locks[i].in_use = false;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].active = false;
}

bool lock_file(const char *filename) {
    // Check if already locked
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_locks[i].in_use && strcmp(file_locks[i].filename, filename) == 0) {
            return false;
        }
    }
    // Find free slot
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_locks[i].in_use) {
            strncpy(file_locks[i].filename, filename, 255);
            file_locks[i].in_use = true;
            return true;
        }
    }
    return false; // No slots
}

void unlock_file(const char *filename) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (file_locks[i].in_use && strcmp(file_locks[i].filename, filename) == 0) {
            file_locks[i].in_use = false;
            return;
        }
    }
}

void send_error(int sockfd, struct sockaddr_in *addr, socklen_t len, uint16_t code, const char *msg) {
    char buf[MAX_BUF];
    uint16_t opcode = htons(5);
    uint16_t err = htons(code);
    memcpy(buf, &opcode, 2);
    memcpy(buf+2, &err, 2);
    int slen = sprintf(buf+4, "%s", msg) + 1 + 4;
    sendto(sockfd, buf, slen, 0, (struct sockaddr*)addr, len);
}

void cleanup_client(int index) {
    if (!clients[index].active) return;
    
    if (clients[index].fp) fclose(clients[index].fp);
    if (clients[index].sockfd > 0) close(clients[index].sockfd);
    
    if (strlen(clients[index].filename) > 0) {
        unlock_file(clients[index].filename);
        printf("[SELECT] Client %d: Closed transfer for '%s'\n", index, clients[index].filename);
    }
    
    clients[index].active = false;
}

// --- Logic ---

void handle_new_request(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    char buffer[MAX_BUF];
    
    ssize_t n = recvfrom(server_fd, buffer, MAX_BUF, 0, (struct sockaddr*)&client_addr, &addr_len);
    if (n < 4) return;
    
    uint16_t opcode = ntohs(*(uint16_t*)buffer);
    
    if (opcode != 1 && opcode != 2) return; // Only RRQ/WRQ

    // Validate packet: Opcode | Filename | 0 | Mode | 0
    char *filename = buffer + 2;
    char *mode = NULL;
    char *end = buffer + n;
    
    char *p = filename;
    while (p < end && *p) p++;
    if (p >= end - 1) {
         // Malformed
         send_error(server_fd, &client_addr, addr_len, 4, "Malformed packet");
         return;
    }
    mode = p + 1;
    p = mode;
    while (p < end && *p) p++;
    if (p >= end) {
         send_error(server_fd, &client_addr, addr_len, 4, "Malformed packet");
         return;
    }
    
    // Validate Mode
    if (strcasecmp(mode, "octet") != 0) {
         send_error(server_fd, &client_addr, addr_len, 4, "Only octet mode supported");
         return;
    }
    
    // Find free client slot
    int cid = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            cid = i;
            break;
        }
    }
    
    if (cid == -1) {
        printf("[SELECT] Server full, dropping request from %s\n", inet_ntoa(client_addr.sin_addr));
        return;
    }
    
    // Create new socket for this client
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }

    // Try to lock file
    if (!lock_file(filename)) {
        printf("[SELECT] File '%s' busy, rejecting.\n", filename);
        send_error(sockfd, &client_addr, addr_len, 0, "File busy"); 
        close(sockfd);
        return;
    }
    
    // Initialize Client Context
    ClientContext *c = &clients[cid];
    c->active = true;
    c->sockfd = sockfd;
    c->client_addr = client_addr;
    c->addr_len = addr_len;
    strncpy(c->filename, filename, 255);
    c->filename[255] = '\0'; // Ensure null-terminated even if long
    c->last_activity = time(NULL);
    c->retries = 0;
    
    char path[512];
    snprintf(path, sizeof(path), REPOSITORY "%s", filename);
    
    if (opcode == 1) { // RRQ (Read Request)
        c->state = STATE_RRQ;
        c->fp = fopen(path, "rb");
        if (!c->fp) {
            send_error(c->sockfd, &c->client_addr, c->addr_len, 1, "File not found");
            cleanup_client(cid);
            return;
        }
        c->block_num = 1;
        
        // Prepare first block
        uint16_t op = htons(3);
        uint16_t blk = htons(1);
        memcpy(c->buffer, &op, 2);
        memcpy(c->buffer+2, &blk, 2);
        size_t bytes = fread(c->buffer+4, 1, 512, c->fp);
        c->buffer_len = bytes + 4;
        
        sendto(c->sockfd, c->buffer, c->buffer_len, 0, (struct sockaddr*)&c->client_addr, c->addr_len);
        printf("[SELECT] Client %d: Started RRQ for '%s'\n", cid, filename);

    } else { // WRQ (Write Request)
        c->state = STATE_WRQ;
        c->fp = NULL; // Will be opened when first DATA block arrives
        c->block_num = 0;
        
        // Send ACK 0
        uint16_t op = htons(4);
        uint16_t blk = htons(0);
        char ack[4];
        memcpy(ack, &op, 2);
        memcpy(ack+2, &blk, 2);
        sendto(c->sockfd, ack, 4, 0, (struct sockaddr*)&c->client_addr, c->addr_len);
        printf("[SELECT] Client %d: '%s'\n Started WRQ for", cid, filename);
    }
}

void handle_client_io(int index) {
    ClientContext *c = &clients[index];
    char recv_buf[MAX_BUF];
    struct sockaddr_in sender;
    socklen_t slen = sizeof(sender);
    
    ssize_t n = recvfrom(c->sockfd, recv_buf, MAX_BUF, 0, (struct sockaddr*)&sender, &slen);
    if (n < 4) return;
    
    // Verify Sender (TID)
    if (sender.sin_addr.s_addr != c->client_addr.sin_addr.s_addr || sender.sin_port != c->client_addr.sin_port) {
        send_error(c->sockfd, &sender, slen, 5, "Unknown transfer ID");
        return;
    }
    
    c->last_activity = time(NULL);
    c->retries = 0; // Reset retries on successful packet
    
    uint16_t opcode = ntohs(*(uint16_t*)recv_buf);
    uint16_t block = ntohs(*(uint16_t*)(recv_buf+2));
    
    if (c->state == STATE_RRQ) {
        // Expecting ACK for c->block_num
        if (opcode == 4 && block == c->block_num) {
            // ACK received for current block.
            // Check if it was the last block (buffer length < 516)
            if (c->buffer_len < 516) {
                printf("[SELECT] Client %d: Transfer complete.\n", index);
                cleanup_client(index);
                return;
            }
            
            // Read next block
            c->block_num++;
            uint16_t op = htons(3);
            uint16_t blk = htons(c->block_num);
            memcpy(c->buffer, &op, 2);
            memcpy(c->buffer+2, &blk, 2);
            size_t bytes = fread(c->buffer+4, 1, 512, c->fp);
            c->buffer_len = bytes + 4;
            
            sendto(c->sockfd, c->buffer, c->buffer_len, 0, (struct sockaddr*)&c->client_addr, c->addr_len);
        }
        else if (opcode == 4 && block == c->block_num - 1) {
             // Duplicate ACK, ignore or retransmit? 
             // Logic says if we receive dup ACK, maybe our data got lost?
             // Usually we just wait for timeout to retransmit.
        }
        
    } else if (c->state == STATE_WRQ) {
        // Expecting DATA with block == c->block_num + 1
        if (opcode == 3) {
            if (block == c->block_num + 1) {
                // First DATA block - open file
                if (!c->fp) {
                    char path[512];
                    snprintf(path, sizeof(path), REPOSITORY "%s", c->filename);
                    c->fp = fopen(path, "wb");
                    if (!c->fp) {
                        send_error(c->sockfd, &c->client_addr, c->addr_len, 2, "Access denied");
                        cleanup_client(index);
                        return;
                    }
                }
                
                // Good block
                fwrite(recv_buf+4, 1, n-4, c->fp);
                c->block_num++;
                
                // Send ACK
                uint16_t op = htons(4);
                uint16_t blk = htons(c->block_num);
                char ack[4];
                memcpy(ack, &op, 2);
                memcpy(ack+2, &blk, 2);
                sendto(c->sockfd, ack, 4, 0, (struct sockaddr*)&c->client_addr, c->addr_len);
                
                if (n < 516) {
                    printf("[SELECT] Client %d: Upload complete.\n", index);
                    cleanup_client(index);
                }
            } else if (block == c->block_num) {
                // Duplicate Data, re-send ACK for prev block
                uint16_t op = htons(4);
                uint16_t blk = htons(c->block_num);
                char ack[4];
                memcpy(ack, &op, 2);
                memcpy(ack+2, &blk, 2);
                sendto(c->sockfd, ack, 4, 0, (struct sockaddr*)&c->client_addr, c->addr_len);
            }
        }
    }
}

void check_timeouts() {
    time_t now = time(NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            if (difftime(now, clients[i].last_activity) >= TFTP_TIMEOUT_SEC) {
                clients[i].retries++;
                if (clients[i].retries > MAX_RETRIES) {
                    printf("[SELECT] Client %d timed out. Aborting.\n", i);
                    cleanup_client(i);
                } else {
                    printf("[SELECT] Client %d timeout. Retrying (%d/%d)...\n", i, clients[i].retries, MAX_RETRIES);
                    // Retransmit logic
                    if (clients[i].state == STATE_RRQ) {
                         // Resend current DATA buffer
                         sendto(clients[i].sockfd, clients[i].buffer, clients[i].buffer_len, 0, (struct sockaddr*)&clients[i].client_addr, clients[i].addr_len);
                    } else if (clients[i].state == STATE_WRQ) {
                        // Resend last ACK
                        uint16_t op = htons(4);
                        uint16_t blk = htons(clients[i].block_num);
                        char ack[4];
                        memcpy(ack, &op, 2);
                        memcpy(ack+2, &blk, 2);
                        sendto(clients[i].sockfd, ack, 4, 0, (struct sockaddr*)&clients[i].client_addr, clients[i].addr_len);
                    }
                    clients[i].last_activity = now;
                }
            }
        }
    }
}

int main() {
    int server_fd;
    struct sockaddr_in server_addr;
    
    init_globals();
    mkdir(REPOSITORY, 0777);

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    
    // Non-blocking server socket? Or just use select.
    // Making it non-blocking shouldn't strictly be necessary if select says it's ready, 
    // but good practice.
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        return 1;
    }

    printf("[SERVER-SELECT] Listening on port %d...\n", PORT);

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        int max_fd = server_fd;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].sockfd, &readfds);
                if (clients[i].sockfd > max_fd) max_fd = clients[i].sockfd;
            }
        }

        struct timeval tv = {1, 0}; // 1 sec timeout for loop to check retransmits
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0 && errno != EINTR) {
            perror("select");
            continue;
        }

        if (activity > 0) {
            if (FD_ISSET(server_fd, &readfds)) {
                handle_new_request(server_fd);
            }
            
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active && FD_ISSET(clients[i].sockfd, &readfds)) {
                    handle_client_io(i);
                }
            }
        }
        
        check_timeouts();
    }

    close(server_fd);
    return 0;
}
