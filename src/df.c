/*
 * Check filesystem usage on an NFS server
 */

#include "nfsping.h"
#include "rpc.h"
#include "util.h"
#include "human.h"
#include <sys/ioctl.h> /* for checking terminal size */


/* we could include the "bytes" in the output string but easier just to keep it in one place */
/* better to keep it in a constant than to have another branch statement in the code */
static const char *header_label[] = {
    [BYTE]  = "bytes",
    [KILO]  = "kbytes",
    [MEGA]  = "mbytes",
    [GIGA]  = "gbytes",
    [TERA]  = "tbytes",
    [PETA]  = "pbytes", /* not used */
    [EXA]   = "ebytes", /* not used */
    /* in human mode we append a prefix to each number, so bytes still makes sense */
    [HUMAN] = "bytes"
};


/* FSSTAT3res returns a size3(uint64) result for inodes which is 18446744073709551615 */
/* that's way too big */
/* it doesn't even make sense as a maximum - that's the same unit for number of available bytes, so each file would be 1 byte */
/* but of course inodes themselves consume space so that's not possible */
/* ext4 has 32 bit inodes */
/* with 32 bit inodes you can have 2^32 per filesystem = 4294967296 = 10 digits */
/* ZFS can have 2^48 inodes (281474976710656) which is 15 digits */
/* Netapp creates 1 inode per 32Kb of filesystem space by default which would be 562949953421311 inodes for a 15EB filesystem */
/* which is also 15 digits */
/* that's a more sensible maximum */

/* let's assume 32 bits is enough for now to keep formatting under control until we see >4 billion inode filesystems in the wild */
static const int max_inode_width = 10;

/* globals */
extern volatile sig_atomic_t quitting;
int verbose = 0;

/* global config "object" */
static struct config {
    uint16_t port;
    unsigned long count;
    int loop;
    enum outputs format;
    enum byte_prefix prefix;
    int inodes;
    int display_ips;
    int one_header;
} cfg;

/* default config */
const struct config CONFIG_DEFAULT = {
    /* default to the standard NFS port */
    .port   = NFS_PORT,
    .count  = 0,
    .loop   = 0,
    .format = unset,
    .prefix = NONE,
    .inodes = 0,
    .display_ips = 0,
    .one_header = 0,
};


/* local prototypes */
static void usage(void);
static FSSTAT3res *get_fsstat(CLIENT *, const char *, nfs_fh_list *);
static void print_header(int, int, enum byte_prefix);
static int print_df(int, char *, char *, FSSTAT3res *, const enum byte_prefix, const unsigned long);
static void print_inodes(int, char *, char *, FSSTAT3res *, const unsigned long);
static char *replace_char(const char *, const char *, const char *);
static void print_format(enum outputs, char *, char *, char *, FSSTAT3res *, const unsigned long, const struct timespec);


void usage() {
    /* TODO:
       -e for exabytes?
       -E for statsd
       -p for petabytes, but that is already used for the graphite prefix
       -g is already used for gigabytes so can't use that for graphite prefix
       -P for the port number (the filehandle comes from nfsmount which doesn't know which port NFS is listening on)
       -V for NFS version
       -J for JSON output
     */
    printf("Usage: nfsdf [options]\n\
    -A         show IP addresses\n\
    -b         display sizes in bytes\n\
    -c n       count of requests to send for each filehandle\n\
    -g         display sizes in gigabytes\n\
    -G         Graphite format output (default human readable)\n\
    -h         display human readable sizes (default)\n\
    -H         frequency in Hertz (requests per second, default %i)\n\
    -i         display inodes\n\
    -k         display sizes in kilobytes\n\
    -l         loop forever\n\
    -m         display sizes in megabytes\n\
    -n         only display the header once\n\
    -M         use the portmapper (default: %i)\n\
    -p string  prefix for graphite metric names\n\
    -S addr    set source address\n\
    -t         display sizes in terabytes\n\
    -T         use TCP (default UDP)\n\
    -v         verbose output\n",
    NFS_HERTZ, NFS_PORT);

    exit(3);
}


FSSTAT3res *get_fsstat(CLIENT *client, const char *host, nfs_fh_list *fs) {
    FSSTAT3args fsstatarg = {
        .fsroot = fs->nfs_fh
    };
    const char *proc = "nfsproc3_fsstat_3";
    struct rpc_err clnt_err;

    FSSTAT3res *fsstatres = nfsproc3_fsstat_3(&fsstatarg, client);

    if (fsstatres) {
        if (fsstatres->status != NFS3_OK) {
            fprintf(stderr, "%s:%s ", host, fs->path);
            clnt_geterr(client, &clnt_err);
            clnt_err.re_status ? clnt_perror(client, proc) : nfs_perror(fsstatres->status, proc);
        }
    } else {
        fprintf(stderr, "%s:%s ", host, fs->path);
        clnt_perror(client, proc);
    }

    return fsstatres;
}


/*
 * print a single header line in "ping" format
 */
/* Netapp style:
   Filesystem          kbytes   used     avail   capacity Mounted on
 */
/* OSX style with inodes:
   Filesystem      Size   Used  Avail Capacity  iused    ifree %iused  Mounted on
 */
/* Netapp inode style:
   Filesystem          iused    ifree    %iused  Mounted on
 */
/* Our format, including inodes and ms for the RPC call time:
   Filesystem       bytes     used    avail capacity      iused      ifree  %iused    ms
 */
/* only allow enough space for 99999 ms (100 seconds) */
/* inode header format, includes an "inodes" column like GNU df for the total:
   Filesystem       inodes      iused      ifree %iused    ms
 */
/* TODO check max line length < 80 or truncate path (or -w option for wide?) */
void print_header(int maxhost, int maxpath, enum byte_prefix prefix) {
    int width = 0;

    if (cfg.format == ping) {
        if (cfg.inodes) {
            printf("%-*s %*s %*s %*s %%iused    ms\n",
                maxhost + maxpath + 1, "Filesystem", max_inode_width, "inodes", max_inode_width, "iused", max_inode_width, "ifree");
        } else {
            width = prefix_width[prefix];
            /* extra space for gap between columns */
            width++;

            /* leave room for unit labels */
            if (prefix == HUMAN) {
                width += 2;
            }

            /* leave enough space for milliseconds */
            printf("%-*s %*s %*s %*s capacity %*s %*s  %%iused    ms\n",
                /* host + export */
                maxhost + maxpath + 1, "Filesystem",
                /* size */
                width, header_label[prefix], width, "used", width, "avail",
                /* inodes */
                max_inode_width, "iused", max_inode_width, "ifree");
        }
    }
}


/* print 'df' style output */
/* Netapp style:
   toaster> df
   Filesystem          kbytes   used     avail   capacity Mounted on
   /vol/vol0           4339168  1777824  2561344 41%      /vol/vol0
 */
/* TODO should it also print inodes by default like OSX's df? The fsstat call already returns those results */
/* OSX style:
   Filesystem      Size   Used  Avail Capacity  iused    ifree %iused  Mounted on
   /dev/disk0s2   112Gi   55Gi   57Gi    50% 14570638 14841730   50%   /
 */
int print_df(int offset, char *host, char *path, FSSTAT3res *fsstatres, const enum byte_prefix prefix, unsigned long usec) {
    /* just use static string arrays, they're not big enough to allocate memory dynamically */
    char total[max_prefix_width];
    char used[max_prefix_width];
    char avail[max_prefix_width];
    double capacity;
    double inode_capacity;
    /* extra space for gap between columns */
    int width = prefix_width[prefix] + 1;

    /* leave room for unit labels */
    if (prefix == HUMAN) {
        width += 2;
    }

    if (fsstatres && fsstatres->status == NFS3_OK) {
        /* format results by prefix */
        /* allow mixed prefixes for a single filesystem */
        prefix_print(fsstatres->FSSTAT3res_u.resok.tbytes, total, prefix);
        prefix_print(fsstatres->FSSTAT3res_u.resok.tbytes - fsstatres->FSSTAT3res_u.resok.fbytes, used, prefix);
        prefix_print(fsstatres->FSSTAT3res_u.resok.fbytes, avail, prefix);

        /* don't use prefix_print() for inodes because the numbers get too small too quickly */
        /* anything above -t gives a <0 */
        /* just print the raw numbers */

        /* there's no total inodes column in Netapp or OSX output */

        /* percent full */
        capacity       = (1 - ((double)fsstatres->FSSTAT3res_u.resok.fbytes / fsstatres->FSSTAT3res_u.resok.tbytes)) * 100;
        inode_capacity = (1 - ((double)fsstatres->FSSTAT3res_u.resok.ffiles / fsstatres->FSSTAT3res_u.resok.tfiles)) * 100;

        /* TODO check usec is less than 100000 (100ms) so column doesn't overflow
         * in that case start losing precision after decimal */

        printf("%s:%-*s %*s %*s %*s %7.0f%% %*" PRIu64 " %*" PRIu64 "  %5.0f%% %#5.2f\n",
            host, offset, path,
            width, total, width, used, width, avail, capacity,
            /* calculate number of used inodes */
            max_inode_width, fsstatres->FSSTAT3res_u.resok.tfiles - fsstatres->FSSTAT3res_u.resok.ffiles,
            max_inode_width, fsstatres->FSSTAT3res_u.resok.ffiles,
            inode_capacity,
            /* RPC call time in milliseconds */
            usec / 1000.0);
    } else {
        /* get_fsstat will print the rpc error */
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}


/* TODO human readable output ie 4M, use the -h flag */
/* GNU df allows -h with inode output */
/* TODO allow all prefixes to work with -i? */
/* base 10 (SI units) vs base 2? (should this be an option for bytes as well?) */
/* Netapp style output:
   toaster> df -i /vol/vol0
   Filesystem          iused    ifree    %iused  Mounted on
   /vol/vol0           164591   14313    92%     /vol/vol0

   (Except right justify columns)
 */
void print_inodes(int offset, char *host, char *path, FSSTAT3res *fsstatres, const unsigned long usec) {
    double capacity;

    /* percent used */
    capacity = (1 - ((double)fsstatres->FSSTAT3res_u.resok.ffiles / fsstatres->FSSTAT3res_u.resok.tfiles)) * 100;

    printf("%s:%-*s %*" PRIu64 " %*" PRIu64 " %*" PRIu64 " %5.0f%% %5.2f\n",
        host,
        offset, path,
        /* total number of inodes on filesystem */
        max_inode_width, fsstatres->FSSTAT3res_u.resok.tfiles,
        /* calculate number of used inodes */
        max_inode_width, fsstatres->FSSTAT3res_u.resok.tfiles - fsstatres->FSSTAT3res_u.resok.ffiles,
        /* free inodes */
        max_inode_width, fsstatres->FSSTAT3res_u.resok.ffiles,
        capacity,
        usec / 1000.0);
}


char *replace_char(const char *str, const char *old, const char *new)
{
    char *ret, *r;
    const char *p, *q;
    size_t oldlen = strlen(old);
    size_t count = 0, retlen, newlen = strlen(new);
    int samesize = (oldlen == newlen);

    if (!samesize) {
        for (count = 0, p = str; (q = strstr(p, old)) != NULL; p = q + oldlen)
            count++;
        /* This is undefined if p - str > PTRDIFF_MAX */
        retlen = p - str + strlen(p) + count * (newlen - oldlen);
    } else
        retlen = strlen(str);

    if ((ret = malloc(retlen + 1)) == NULL)
        return NULL;

    r = ret, p = str;
    while (1) {
        if (!samesize && !count--)
            break;
        if ((q = strstr(p, old)) == NULL)
            break;
        ptrdiff_t l = q - p;
        memcpy(r, p, l);
        r += l;
        memcpy(r, new, newlen);
        r += newlen;
        p = q + oldlen;
    }
    strcpy(r, p);

    return ret;
}

/* formatted output ie graphite */
/* TODO escape dots and spaces (replace with underscores) in paths */
void print_format(enum outputs format, char *prefix, char *ndqf, char *path, FSSTAT3res *fsstatres, const unsigned long usec, const struct timespec now) {
    char *bad_characters[] = {
        " ", ".", "-", "/"
    };
    int index = 0;
    int number_of_chars = sizeof(bad_characters) / sizeof(bad_characters[0]);

    for (index = 0; index < number_of_chars; index++) {
        path = replace_char(path, bad_characters[index], "_");
    }

    /* TODO round seconds up to next whole second? */
    switch (format) {
        case graphite:
            /* TODO just one printf! */
            printf("%s.%s.df.%s.tbytes %" PRIu64 " %li\n", prefix, ndqf, path, fsstatres->FSSTAT3res_u.resok.tbytes, now.tv_sec);
            printf("%s.%s.df.%s.fbytes %" PRIu64 " %li\n", prefix, ndqf, path, fsstatres->FSSTAT3res_u.resok.fbytes, now.tv_sec);
            printf("%s.%s.df.%s.tfiles %" PRIu64 " %li\n", prefix, ndqf, path, fsstatres->FSSTAT3res_u.resok.tfiles, now.tv_sec);
            printf("%s.%s.df.%s.ffiles %" PRIu64 " %li\n", prefix, ndqf, path, fsstatres->FSSTAT3res_u.resok.ffiles, now.tv_sec);
            printf("%s.%s.df.%s.usec %lu %li\n", prefix, ndqf, path, usec, now.tv_sec);
            break;
        default:
            fatal("Unsupported format\n");
    }
}


int main(int argc, char **argv) {
    int ch;
    char output_prefix[255] = "nfs";
    char *input_fh = NULL;
    size_t n = 0; /* for getline() */
    targets_t dummy = { 0 };
    targets_t *current = &dummy;
    targets_t *targets = current;
    nfs_fh_list *filehandle;
    unsigned int maxpath = 0;
    unsigned int maxhost = 0;
    struct winsize winsz;
    unsigned short rows  = 0; /* number of rows in terminal window */
    unsigned long version = 3;
    struct timespec loop_start, loop_end, loop_elapsed, sleepy;
    /* default to 1Hz */
    struct timespec sleep_time = {
        .tv_sec = 1,
        .tv_nsec = 0
    };
    unsigned long hertz       = NFS_HERTZ;
    struct timeval timeout    = NFS_TIMEOUT;
    struct timespec call_start, call_end, call_elapsed, wall_clock;
    unsigned long usec = 0;
    /* count of requests sent */
    unsigned long df_sent = 0;
    /* count of successful requests */
    unsigned long df_ok   = 0;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        /* default to UDP */
        .ai_socktype = SOCK_DGRAM,
    };
    FSSTAT3res *fsstatres;
    /* source ip address for packets */
    struct sockaddr_in src_ip = {
        .sin_family = AF_INET,
        .sin_addr = 0
    };

    /* set the default config "object" */
    cfg = CONFIG_DEFAULT;

    while ((ch = getopt(argc, argv, "Abc:gGhH:iklmMnp:S:tTv")) != -1) {
        switch(ch) {
            /* display IP addresses */
            case 'A':
                cfg.display_ips = 1;
                break;
            /* display bytes */
            case 'b':
                if (cfg.prefix == NONE) {
                    cfg.prefix = BYTE;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* loop count */
            case 'c':
                if (cfg.loop) {
                    fatal("Can't specify both -l and -c!\n");
                }
                cfg.count = strtoul(optarg, NULL, 10);
                if (cfg.count == 0 || cfg.count == ULONG_MAX) {
                   fatal("Zero count, nothing to do!\n");
                }
                if (cfg.format == unset) {
                    cfg.format = ping;
                }
                break;
            /* display gigabytes */
            case 'g':
                if (cfg.prefix == NONE) {
                    cfg.prefix = GIGA;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* Graphite output */
            case 'G':
                if (cfg.prefix == NONE) {
                    cfg.format = graphite;
                } else {
                    fatal("Can't specify units and -G!\n");
                }
                break;
            /* display human readable (default, set below) */
            case 'h':
                if (cfg.prefix == NONE) {
                    cfg.prefix = HUMAN;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* polling frequency */
            case 'H':
                /* TODO check for reasonable values */
                hertz = strtoul(optarg, NULL, 10);
                break;
            /* display inodes */
            case 'i':
                cfg.inodes = 1; 
                break;
            /* display kilobytes */
            case 'k':
                if (cfg.prefix == NONE) {
                    cfg.prefix = KILO;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* loop forever */
            case 'l':
                if (cfg.count) {
                    fatal("Can't specify both -c and -l!\n");
                }
                cfg.loop = 1;
                if (cfg.format == unset) {
                    cfg.format = ping;
                }
                break;
            /* display megabytes */
            case 'm':
                if (cfg.prefix == NONE) {
                    cfg.prefix = MEGA;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* portmapper */
            case 'M':
                cfg.port = 0;
                break;
            /* just display the header once */
            case 'n':
                cfg.one_header = 1;
                break;
            /* prefix to use for graphite metrics */
            case 'p':
                strncpy(output_prefix, optarg, sizeof(output_prefix));
                break;
            /* specify source address */
            case 'S':
                if (inet_pton(AF_INET, optarg, &src_ip.sin_addr) != 1) {
                    fatal("nfsping: Invalid source IP address!\n");
                }
                break;
            /* display terabytes */
            case 't':
                if (cfg.prefix == NONE) {
                    cfg.prefix = TERA;
                } else {
                    fatal("Can't specify multiple units!\n");
                }
                break;
            /* use TCP */
            case 'T':
                hints.ai_socktype = SOCK_STREAM;
                break;
            /* verbose */
            case 'v':
                verbose = 1;
                break;
            /* have to keep -h available for human readable output */
            case '?':
            default:
                usage();
        }
    }

    /* default to human readable */
    if (cfg.prefix == NONE) {
        cfg.prefix = HUMAN;
    }

    /* default output */
    if (cfg.format == unset) {
        cfg.format = ping;
    }

    /* calculate the sleep_time based on the frequency */
    /* this doesn't support frequencies lower than 1Hz */
    if (hertz > 1) {
        sleep_time.tv_sec = 0;
        /* nanoseconds */
        sleep_time.tv_nsec = 1000000000 / hertz;
    }

    
    /* first parse all of the input filehandles into a list  from stdin, one per line
     * this gives us the longest path so we can lay out the output
     * TODO only for human readable output, otherwise do it line by line
     */
    while (getline(&input_fh, &n, stdin) != -1) {

        /* don't allocate space for results */
        current = parse_fh(targets, input_fh, cfg.port, timeout, 0);

        /* save the longest host/paths for display formatting */
        if (current) {
            if (strlen(current->filehandles->path) > maxpath) {
                maxpath = strlen(current->filehandles->path);
            }

            /* check if we're displaying hostnames or IP addresses */
            if (cfg.display_ips) {
                if (strlen(current->ip_address) > maxhost) {
                    maxhost = strlen(current->ip_address);
                }
            } else {
                if (strlen(current->name) > maxhost) {
                    maxhost = strlen(current->name);
                }
            }
        }
    }

    /* set to start of list, skipping first dummy entry */
    targets = targets->next;

    /* 
     * Print the header before sending any RPCs, this means we have to guess about the size of the results
     * but it lets the user know that the program is running. Then we can print the results as they come in
     * which will also give a visual indication of which filesystems are slow to respond. We also may be
     * looping or running for a specific count, so we can't wait for just the first result to size the 
     * columns since the results may change over time.
     */

    /* always print one header at the start */
    print_header(maxhost, maxpath, cfg.prefix);

    /* listen for ctrl-c */
    quitting = 0;
    signal(SIGINT, sigint_handler);

    /* the main loop */
    while(1) {
        /* find the current number of rows in the terminal for printing the header once per screen */
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz);
        rows = winsz.ws_row;

        /* reset to start of list */
        current = targets;

#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &loop_start);
#else  
        clock_gettime(CLOCK_MONOTONIC, &loop_start);
#endif 

        while (current) {
            /* make a new connection if needed */
            if (current->client == NULL) {
                current->client = create_rpc_client(current->client_sock, &hints, NFS_PROGRAM, version, timeout, src_ip);
                /* don't use default AUTH_NONE */
                auth_destroy(current->client->cl_auth);
                /* set up AUTH_SYS instead */
                current->client->cl_auth = authunix_create_default();
            }

            /* loop through the filehandles for each target */

            filehandle = current->filehandles;

            while (filehandle) {

                /* get the current timestamp */
                clock_gettime(CLOCK_REALTIME, &wall_clock);

#ifdef CLOCK_MONOTONIC_RAW
                clock_gettime(CLOCK_MONOTONIC_RAW, &call_start);
#else  
                clock_gettime(CLOCK_MONOTONIC, &call_start);
#endif 
                /* the actual RPC call */
                fsstatres = get_fsstat(current->client, current->name, filehandle);
                /* second time marker */
#ifdef CLOCK_MONOTONIC_RAW
                clock_gettime(CLOCK_MONOTONIC_RAW, &call_end);
#else  
                clock_gettime(CLOCK_MONOTONIC, &call_end);
#endif
                df_sent++;
                filehandle->sent++;

                /* calculate elapsed microseconds */
                timespecsub(&call_end, &call_start, &call_elapsed);
                usec = ts2us(call_elapsed);

                if (fsstatres && fsstatres->status == NFS3_OK) {
                    df_ok++;
                    current->received++;

                    /* print header once per screen like vmstat */
                    /* TODO maybe a better number than df_ok? What about errors? Or the header line itself? */
                    if (cfg.one_header == 0 && (df_ok % rows == 0)) {
                        print_header(maxhost, maxpath, cfg.prefix);
                    }

                    if (cfg.format == ping) {
                        if (cfg.inodes) {
                            if (cfg.display_ips) {
                                print_inodes(maxpath, current->ip_address, filehandle->path, fsstatres, usec);
                            } else {
                                print_inodes(maxpath, current->name, filehandle->path, fsstatres, usec);
                            }
                        } else {
                            /* are we printing ip_addresses or hostnames */
                            /* TODO move this logic into print_df? But then have to pass in the filehandle struct */
                            if (cfg.display_ips) {
                                print_df(maxpath, current->ip_address, filehandle->path, fsstatres, cfg.prefix, usec);
                            } else {
                                print_df(maxpath, current->name, filehandle->path, fsstatres, cfg.prefix, usec);
                            }
                        }
                    } else {
                        print_format(cfg.format, output_prefix, current->ndqf, filehandle->path, fsstatres, usec, wall_clock);
                    }
                }

                /* free the result */
                xdr_free((xdrproc_t)xdr_FSSTAT3res, (char *)fsstatres);

                /* TODO pause between requests to same target? */

                filehandle = filehandle->next;
            } /* while(filehandle) */

            /* don't pause between targets */

            current = current->next;
        } /* while(current) */

        /* measure how long the current round took, and subtract that from the sleep time */
        /* this keeps us on the polling frequency */
#ifdef CLOCK_MONOTONIC_RAW
        clock_gettime(CLOCK_MONOTONIC_RAW, &loop_end);
#else
        clock_gettime(CLOCK_MONOTONIC, &loop_end);
#endif
        timespecsub(&loop_end, &loop_start, &loop_elapsed);
        debug("Polling took %lld.%.9lds\n", (long long)loop_elapsed.tv_sec, loop_elapsed.tv_nsec);

        /* ctrl-c */
        if (quitting) {
            break;
        }

        /* only sleep if looping or counting */
        /* check the count against the first filehandle in the first target */
        if (cfg.loop || (cfg.count && targets->filehandles->sent < cfg.count)) {
            /* don't sleep if we went over the sleep_time */
            if (timespeccmp(&loop_elapsed, &sleep_time, >)) {
                debug("Slow poll, not sleeping\n");
            /* sleep between rounds */
            } else {
                timespecsub(&sleep_time, &loop_elapsed, &sleepy);
                debug("Sleeping for %lld.%.9lds\n", (long long)sleepy.tv_sec, sleepy.tv_nsec);
                nanosleep(&sleepy, NULL);
            }
        } else {
            break;
        }
    } /* while (1) */

    /* check if all the results came back ok */
    if (df_sent && df_sent == df_ok) {
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
