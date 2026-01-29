#ifndef COMMON_H
#define COMMON_H

/* Standard System Libraries */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

/* Project Specific Headers */
#include "protocol.h"

/* Global Constants */
#define DEFAULT_MODE "octet"
#define TIMEOUT_SEC  5

#endif