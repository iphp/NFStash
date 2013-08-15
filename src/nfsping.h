#ifndef NFSPING_H
#define NFSPING_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <time.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

/* local copies */
#include "nfs_prot.h"
#include "mount.h"

/* struct timeval */
#define NFS_TIMEOUT { 2, 500000 };
/* struct timespec */
#define NFS_WAIT { 0, 25000000 };
/* struct timespec */
#define NFS_SLEEP { 1, 0 };

/* filehandle string length, including IP address, path string, colon separators and NUL */
/* xxx.xxx.xxx.xxx:/path:hhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh\0 */
/* this allows for 64 byte filehandles but most are 32 byte */
#define FHMAX 16 + 1 + MNTPATHLEN + 1 + FHSIZE3 * 2 + 1

/* for shifting */
#define KILO 10
#define MEGA 20
#define GIGA 30
#define TERA 40

typedef struct targets {
    char *name;
    char *ndqf; /* reversed name */
    struct sockaddr_in *client_sock;
    CLIENT *client;
    unsigned long *results;
    struct targets *next;
    unsigned int sent, received;
    float min, max, avg;
} targets_t;

typedef struct fsroots {
    char *host;
    struct sockaddr_in *client_sock;
    char *path;
    nfs_fh3 fsroot;
    struct fsroots *next;
} fsroots_t;

enum outputs {
    human,
    fping,
    graphite,
    statsd,
    opentsdb,
    unixtime
};

#endif /* NFSPING_H */
