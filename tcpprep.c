/* $Id: tcpprep.c,v 1.35 2004/04/22 23:46:39 aturner Exp $ */

/*
 * Copyright (c) 2001-2004 Aaron Turner.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright owners nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 *  Purpose:
 *  1) Remove the performance bottleneck in tcpreplay for choosing an NIC
 *  2) Seperate code to make it more manageable
 *  3) Add addtional features which require multiple passes of a pcap
 *
 *  Support:
 *  Right now we support matching source IP based upon on of the following:
 *  - Regular expression
 *  - IP address is contained in one of a list of CIDR blocks
 *  - Auto learning of CIDR block for servers (clients all other)
 */

#include <libnet.h>
#include <pcap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include "tcpreplay.h"
#include "cache.h"
#include "cidr.h"
#include "tcpprep.h"
#include "tree.h"
#include "list.h"
#include "xX.h"
#include "err.h"
#include "rbtree.h"
#include "utils.h"

/*
 * global variables
 */
#ifdef DEBUG
int debug = 0;
#endif
int info = 0;
char *ourregex = NULL;
char *cidr = NULL;
regex_t *preg = NULL;
CIDR *cidrdata = NULL;
CACHE *cachedata = NULL;
struct data_tree treeroot;
struct options options;
struct bpf_program bpf;

char tcpservices[NUM_PORTS], udpservices[NUM_PORTS];
int mode = 0;
int automode = 0;
double ratio = 0.0;
int max_mask = DEF_MAX_MASK;
int min_mask = DEF_MIN_MASK;
extern char *optarg;
extern int optind, opterr, optopt;

int include_exclude_mode;
CIDR *xX_cidr = NULL;
LIST *xX_list = NULL;

/* required to include utils.c */
int non_ip = 0;
int maxpacket = 0; 

/* we get this from libpcap */
extern char pcap_version[];

static void usage();
static void version();
static int check_ip_regex(const unsigned long ip);
static unsigned long process_raw_packets(pcap_t * pcap);
static int check_dst_port(ip_hdr_t *ip_hdr, int len);
static void parse_services(char *file);

static void
version()
{
    fprintf(stderr, "tcpprep version: %s", VERSION);
#ifdef DEBUG
    fprintf(stderr, " (debug)\n");
#else
    fprintf(stderr, "\n");
#endif
    fprintf(stderr, "Cache file supported: %s\n", CACHEVERSION);
    fprintf(stderr, "Compiled against libnet: %s\n", LIBNET_VERSION);
    fprintf(stderr, "Compiled against libpcap: %s\n", pcap_version);
    exit(0);
}

/*
 *  usage
 */
static void
usage()
{
    fprintf(stderr, "Usage: tcpprep [-a -n <mode> -N <type> | -c <cidr> | -p | -r <regex>] \\\n\t\t-o <out> -i <in> <args>\n");
    fprintf(stderr, "-a\t\t\tSplit traffic in Auto Mode\n"
            "-c CIDR1,CIDR2,...\tSplit traffic in CIDR Mode\n"
            "-C <comment>\t\tEmbed comment in tcpprep cache file\n");
#ifdef DEBUG
    fprintf(stderr, "-d <level>\t\tEnable debug output to STDERR\n");
#endif
    fprintf(stderr, "-h\t\t\tHelp\n"
            "-i <capfile>\t\tInput capture file to process\n"
            "-m <minmask>\t\tMinimum mask length in Auto/Router mode\n"
            "-M <maxmask>\t\tMaximum mask length in Auto/Router mode\n"
            "-n <auto mode>\t\tUse specified algorithm in Auto Mode\n"
            "-N client|server\tClassify non-IP traffic as client/server\n"
            "-o <outputfile>\t\tOutput cache file name\n"
            "-p\t\t\tSplit traffic based on destination port\n"
            "-P <file>\t\tPrint comment in tcpprep file\n");
    fprintf(stderr, "-r <regex>\t\tSplit traffic in Regex Mode\n"
            "-R <ratio>\t\tSpecify a ratio to use in Auto Mode\n"
            "-s <file>\t\tSpecify service ports in /etc/services format\n"
            "-x <match>\t\tOnly send the packets specified\n"
            "-X <match>\t\tSend all the packets except those specified\n"
            "-v\t\t\tVerbose\n" 
            "-V\t\t\tVersion\n");
    exit(0);
}

static void
print_comment(char *file)
{
    char *cachedata = NULL;

    read_cache(&cachedata, file);
    printf("Comment:\n%s\n", options.tcpprep_comment);

    exit(0);
}

/*
 * checks the dst port to see if this is destined for a server port.
 * returns 1 for true, 0 for false
 */
static int 
check_dst_port(ip_hdr_t *ip_hdr, int len)
{
    tcp_hdr_t *tcp_hdr = NULL;
    udp_hdr_t *udp_hdr = NULL;

    dbg(3, "Checking the destination port...");

    if (ip_hdr->ip_p == IPPROTO_TCP) {
        tcp_hdr = (tcp_hdr_t *)get_layer4(ip_hdr);

        /* is a service? */
        if (tcpservices[ntohs(tcp_hdr->th_dport)]) {
            dbg(1, "TCP packet is destined for a server port: %d", ntohs(tcp_hdr->th_dport));
            return 1;
        }

        /* nope */
        dbg(1, "TCP packet is NOT destined for a server port: %d", ntohs(tcp_hdr->th_dport));
        return 0;
    } else if (ip_hdr->ip_p == IPPROTO_UDP) {
        udp_hdr = (udp_hdr_t *)get_layer4(ip_hdr);

        /* is a service? */
        if (udpservices[ntohs(udp_hdr->uh_dport)]) {
            dbg(1, "UDP packet is destined for a server port: %d", ntohs(udp_hdr->uh_dport));
            return 1;
        }

        /* nope */
        dbg(1, "UDP packet is NOT destined for a server port: %d", ntohs(udp_hdr->uh_dport));
        return 0;
    }

    
    /* not a TCP or UDP packet... return as non_ip */
    dbg(1, "Packet isn't a UDP or TCP packet... no port to process.");
    return non_ip;
}


/*
 * checks to see if an ip address matches a regex.  Returns 1 for true
 * 0 for false
 */
static int
check_ip_regex(const unsigned long ip)
{
    int eflags = 0;
    u_char src_ip[16];
    size_t nmatch = 0;
    regmatch_t *pmatch = NULL;

    memset(src_ip, '\0', 16);
    strncat((char *)src_ip, (char *)libnet_addr2name4(ip, LIBNET_DONT_RESOLVE),
            15);
    if (regexec(preg, (char *)src_ip, nmatch, pmatch, eflags) == 0) {
        return (1);
    }
    else {
        return (0);
    }

}

/*
 * uses libpcap library to parse the packets and build
 * the cache file.
 */
static unsigned long
process_raw_packets(pcap_t * pcap)
{
    ip_hdr_t *ip_hdr = NULL;
    eth_hdr_t *eth_hdr = NULL;
    struct pcap_pkthdr pkthdr;
    const u_char *pktdata = NULL;
    unsigned long packetnum = 0;
#ifdef FORCE_ALIGN
    u_char ipbuff[MAXPACKET];
#endif

    while ((pktdata = pcap_next(pcap, &pkthdr)) != NULL) {
        packetnum++;
        eth_hdr = (eth_hdr_t *) pktdata;

        dbg(1, "Packet %d", packetnum);

        /* look for include or exclude LIST match */
        if (xX_list != NULL) {
            if (include_exclude_mode < xXExclude) {
                if (!check_list(xX_list, (packetnum))) {
                    add_cache(&cachedata, 0, 0);
                    continue;
                }
            }
            else if (check_list(xX_list, (packetnum))) {
                add_cache(&cachedata, 0, 0);
                continue;
            }
        }

        if (ntohs(eth_hdr->ether_type) != ETHERTYPE_IP) {
            dbg(2, "Packet isn't IP: 0x%.2x", eth_hdr->ether_type);

            if (mode != AUTO_MODE)  /* we don't want to cache
                                     * these packets twice */
                add_cache(&cachedata, 1, non_ip);
            continue;
        }

#ifdef FORCE_ALIGN
        /* 
         * copy layer 3 and up to our temp packet buffer
         * for now on, we have to edit the packetbuff because
         * just before we send the packet, we copy the packetbuff 
         * back onto the pkt.data + LIBNET_ETH_H buffer
         * we do all this work to prevent byte alignment issues
         */
        ip_hdr = (ip_hdr_t *) & ipbuff;
        memcpy(ip_hdr, (pktdata + LIBNET_ETH_H), (pkthdr.caplen - LIBNET_ETH_H));
#else
        /*
         * on non-strict byte align systems, don't need to memcpy(), 
         * just point to 14 bytes into the existing buffer
         */
        ip_hdr = (ip_hdr_t *) (pktdata + LIBNET_ETH_H);
#endif

        /* look for include or exclude CIDR match */
        if (xX_cidr != NULL) {
            if (!process_xX_by_cidr(include_exclude_mode, xX_cidr, ip_hdr)) {
                add_cache(&cachedata, 0, 0);
                continue;
            }
        }

        switch (mode) {
        case REGEX_MODE:
            add_cache(&cachedata, 1, check_ip_regex(ip_hdr->ip_src.s_addr));
            break;
        case CIDR_MODE:
            add_cache(&cachedata, 1,
                      check_ip_CIDR(cidrdata, ip_hdr->ip_src.s_addr));
            break;
        case AUTO_MODE:
            /* first run through in auto mode: create tree */
            add_tree(ip_hdr->ip_src.s_addr, pktdata);
            break;
        case ROUTER_MODE:
            add_cache(&cachedata, 1,
                      check_ip_CIDR(cidrdata, ip_hdr->ip_src.s_addr));
            break;
        case BRIDGE_MODE:
            /*
             * second run through in auto mode: create bridge
             * based cache
             */
            add_cache(&cachedata, 1,
                      check_ip_tree(UNKNOWN, ip_hdr->ip_src.s_addr));
            break;
        case SERVER_MODE:
            /* 
             * second run through in auto mode: create bridge
             * where unknowns are servers
             */
            add_cache(&cachedata, 1,
                      check_ip_tree(SERVER, ip_hdr->ip_src.s_addr));
            break;
        case CLIENT_MODE:
            /* 
             * second run through in auto mode: create bridge
             * where unknowns are clients
             */
            add_cache(&cachedata, 1,
                      check_ip_tree(CLIENT, ip_hdr->ip_src.s_addr));
            break;
        case PORT_MODE:
            /*
             * process ports based on their destination port
             */
            add_cache(&cachedata, 1, 
                      check_dst_port(ip_hdr, (pkthdr.caplen - LIBNET_ETH_H)));
            break;
        }

    }

    return packetnum;
}

/*
 *  main()
 */
int
main(int argc, char *argv[])
{
    int out_file, ch, regex_error = 0, mask_count = 0;
    int regex_flags = REG_EXTENDED|REG_NOSUB;
    int i;
    char *infilename = NULL;
    char *outfilename = NULL;
    char ebuf[EBUF_SIZE];
    u_int64_t totpackets = 0;
    void *xX = NULL;
    pcap_t *pcap = NULL;
    char errbuf[PCAP_ERRBUF_SIZE];

    memset(&options, '\0', sizeof(options));
    options.bpf_optimize = BPF_OPTIMIZE;

    preg = (regex_t *) malloc(sizeof(regex_t));
    if (preg == NULL)
        err(1, "malloc");

    /* set default server ports (override w/ -s) */
    memset(tcpservices, '\0', NUM_PORTS);
    memset(udpservices, '\0', NUM_PORTS);
    for (i = DEFAULT_LOW_SERVER_PORT; i <= DEFAULT_HIGH_SERVER_PORT; i++) {
        tcpservices[i] = 1;
        udpservices[i] = 1;
    }

#ifdef DEBUG
    while ((ch = getopt(argc, argv, "ad:c:C:r:R:o:pP:i:hm:M:n:N:s:x:X:vV")) != -1)
#else
    while ((ch = getopt(argc, argv, "ac:C:r:R:o:pP:i:hm:M:n:N:s:x:X:vV")) != -1)
#endif
        switch (ch) {
        case 'a':
            mode = AUTO_MODE;
            break;
        case 'c':
            if (!parse_cidr(&cidrdata, optarg, ",")) {
                usage();
            }
            mode = CIDR_MODE;
            break;
        case 'C':
            options.tcpprep_comment[COMMENT_LEN -1] = '\0';
            strncpy(options.tcpprep_comment, optarg, COMMENT_LEN -1);
            dbg(1, "comment length: %d", strlen(options.tcpprep_comment));
            break;
#ifdef DEBUG
        case 'd':
            debug = atoi(optarg);
            break;
#endif
        case 'h':
            usage();
            break;
        case 'i':
            infilename = optarg;
            break;
        case 'm':
            min_mask = atoi(optarg);
            mask_count++;
            break;
        case 'M':
            max_mask = atoi(optarg);
            mask_count++;
            break;
        case 'n':
            if (strcmp(optarg, "bridge") == 0) {
                automode = BRIDGE_MODE;
            }
            else if (strcmp(optarg, "router") == 0) {
                automode = ROUTER_MODE;
            }
            else if (strcmp(optarg, "client") == 0) {
                automode = CLIENT_MODE;
            }
            else if (strcmp(optarg, "server") == 0) {
                automode = SERVER_MODE;
            }
            else {
                errx(1, "Invalid network type: %s", optarg);
            }
            break;
        case 'N':
            if (strcmp(optarg, "client") == 0) {
                non_ip = 0;
            }
            else if (strcmp(optarg, "server") == 0) {
                non_ip = 1;
            }
            else {
                errx(1, "-N must be client or server");
            }
            break;
        case 'o':
            outfilename = optarg;
            break;
        case 'p':
            mode = PORT_MODE;
            break;
        case 'P':
            print_comment(optarg);
            /* exits */
            break;
        case 'r':
            ourregex = optarg;
            mode = REGEX_MODE;
            if ((regex_error = regcomp(preg, ourregex, regex_flags))) {
                if (regerror(regex_error, preg, ebuf, EBUF_SIZE) != -1) {
                    errx(1, "Error compiling regex: %s", ebuf);
                }
                else {
                    errx(1, "Error compiling regex.");
                }
                exit(1);
            }
            break;
        case 'R':
            ratio = atof(optarg);
            break;
        case 's':
            printf("Parsing services...\n");
            parse_services(optarg);
            break;
        case 'x':
            include_exclude_mode = 'x';
            if ((xX = parse_xX_str(include_exclude_mode, optarg)) == NULL)
                errx(1, "Unable to parse -x: %s", optarg);
            if (include_exclude_mode & xXPacket) {
                xX_list = (LIST *) xX;
            }
            else if (! (include_exclude_mode & xXBPF)) {
                xX_cidr = (CIDR *) xX;
            }
            break;
        case 'X':
            include_exclude_mode = 'X';
            if ((xX = parse_xX_str(include_exclude_mode, optarg)) == NULL)
                errx(1, "Unable to parse -X: %s", optarg);
            if (include_exclude_mode & xXPacket) {
                xX_list = (LIST *) xX;
            }
            else {
                xX_cidr = (CIDR *) xX;
            }
            break;
        case 'v':
            info = 1;
            break;
        case 'V':
            version();
            break;
        default:
            usage();
        }

    /* process args */
    if ((mode != CIDR_MODE) && (mode != REGEX_MODE) && 
        (mode != AUTO_MODE) && (mode != PORT_MODE))
        errx(1, "You need to specifiy a vaild CIDR list or regex, or choose auto or port mode");

    if ((mask_count > 0) && (mode != AUTO_MODE))
        errx(1,
             "You can't specify a min/max mask length unless you use auto mode");

    if ((mode == AUTO_MODE) && (automode == 0))
        errx(1,
             "You must specify -n (bridge|router|client|server) with auto mode (-a)");

    if ((ratio != 0.0) && (mode != AUTO_MODE))
        errx(1, "Ratio (-R) only works in auto mode (-a).");

    if (ratio < 0)
        errx(1, "Ratio must be a non-negative number.");

    if (info && mode == AUTO_MODE)
        fprintf(stderr, "Building auto mode pre-cache data structure...\n");

    if (info && mode == CIDR_MODE)
        fprintf(stderr, "Building cache file from CIDR list...\n");

    if (info && mode == REGEX_MODE)
        fprintf(stderr, "Building cache file from regex...\n");

    if (infilename == NULL)
        errx(1, "You must specify a pcap file to read via -i");

    /* set ratio to the default if unspecified */
    if (ratio == 0.0)
        ratio = DEF_RATIO;

    /* open the cache file */
    out_file =
        open(outfilename, O_WRONLY | O_CREAT | O_TRUNC,
             S_IREAD | S_IWRITE | S_IRGRP | S_IWGRP | S_IROTH);
    if (out_file == -1)
        err(1, "Unable to open cache file %s for writing.", outfilename);

  readpcap:
    /* open the pcap file */
    if ((pcap = pcap_open_offline(infilename, errbuf)) == NULL) {
        errx(1, "Error opening file: %s", errbuf);
    }

    /* do we apply a bpf filter? */
    if (options.bpf_filter != NULL) {
        if (pcap_compile(pcap, &bpf, options.bpf_filter,
                         options.bpf_optimize, 0) != 0) {
            errx(1, "Error compiling BPF filter: %s", pcap_geterr(pcap));
        }
        pcap_setfilter(pcap, &bpf);
    }

    if ((totpackets = process_raw_packets(pcap)) == 0) {
        pcap_close(pcap);
        errx(1, "Error: no packets were processed.  Filter too limiting?");
    }
    pcap_close(pcap);


    /* we need to process the pcap file twice in HASH/AUTO mode */
    if (mode == AUTO_MODE) {
        mode = automode;
        if (mode == ROUTER_MODE) {  /* do we need to convert TREE->CIDR? */
            if (info)
                fprintf(stderr, "Building network list from pre-cache...\n");
            if (!process_tree()) {
                errx(1,
                     "Error: unable to build a valid list of servers. Aborting.");
            }
        }
        else {
            /*
             * in bridge mode we need to calculate client/sever
             * manually since this is done automatically in
             * process_tree()
             */
            tree_calculate(&treeroot);
        }

        if (info)
            fprintf(stderr, "Buliding cache file...\n");
        /* 
         * re-process files, but this time generate
         * cache 
         */
        goto readpcap;
    }
#ifdef DEBUG
    if (debug && (cidrdata != NULL))
        print_cidr(cidrdata);
#endif

    /* write cache data */
    totpackets = write_cache(cachedata, out_file, totpackets);
    if (info)
        fprintf(stderr, "Done.\nCached %llu packets.\n", totpackets);

    /* close cache file */
    close(out_file);
    return 0;

}


/*
 * parses /etc/services so we know which ports are service ports
 */
void
parse_services(char *file)
{
    FILE *service = NULL;
    char service_line[MAXLINE], port[10], proto[10];
    regex_t preg;
    int portc;
    size_t nmatch = 3;
    regmatch_t pmatch[3];
    char regex[] = "([0-9]+)/(tcp|udp)"; /* matches the port as pmatch[1], service pmatch[2] */

    dbg(1, "Parsing %s", file);
    memset(service_line, '\0', MAXLINE);

    /* mark all ports not a service */
    memset(tcpservices, '\0', NUM_PORTS);
    memset(udpservices, '\0', NUM_PORTS);

    if ((service = fopen(file, "r")) == NULL) {
        errx(1, "Unable to open service file: %s\n%s", file, strerror(errno));
    }
    
    /* compile our regexes */
    if ((regcomp(&preg, regex, REG_ICASE|REG_EXTENDED)) != 0) {
        errx(1, "Unable to compile regex: %s", regex);
    }

    /* parse the entire file */
    while ((fgets(service_line, MAXLINE, service)) != NULL) {
        /* zero out our vars */
        memset(port, '\0', 10);
        memset(proto, '\0', 10);
        portc = 0;
        
        dbg(4, "Procesing: %s", service_line);
        
        /* look for format of 1234/tcp */
        if ((regexec(&preg, service_line, nmatch, pmatch, 0)) == 0) { /* matches */
            if (nmatch < 2) {
                errx(1, "WTF?  I matched the line, but I don't know where!");
            }

            /* strip out the port & proto from the line */
            strncpy(port, &service_line[pmatch[1].rm_so], (pmatch[1].rm_eo - pmatch[1].rm_so));
            strncpy(proto, &service_line[pmatch[2].rm_so], (pmatch[2].rm_eo - pmatch[2].rm_so));

            /* convert port[] into an integer */
            portc = atoi(port);

            /* update appropriate service array with the server port */
            if (strcmp(proto, "tcp") == 0) {
                dbg(3, "Setting TCP/%d as a server port", portc);
                tcpservices[portc] = 1; /* mark it as a service port */
            } else if (strcmp(proto, "udp") == 0) {
                dbg(3, "Setting UDP/%d as a server port", portc);
                udpservices[portc] = 1;
            } else {
                warnx("Skipping unknown protocol service %s/%d", proto, portc);
            }
        }
    }
}
