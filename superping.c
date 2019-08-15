/**
 * Copyright (C) 2011 CEGO ApS
 * Written by Robert Larsen <robert@komogvind.dk> for CEGO ApS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>

#define WRITE_BUFFER_SIZE (sizeof(struct icmphdr) + sizeof(struct timeval))
#define READ_BUFFER_SIZE (sizeof(struct iphdr) + sizeof(struct icmphdr) + sizeof(struct timeval))
#define ICMP_DATA(b) (&b[sizeof(struct icmphdr)])

struct {
    struct sockaddr_in AddressToPing;
    int WaitTime;
    int RawSocket;
    int EpollFd;
    int Verbose;
    int PrintResponseTime;
    char ReadBuffer[READ_BUFFER_SIZE];
    char WriteBuffer[WRITE_BUFFER_SIZE];
    struct timeval RequestTime;
    enum {
        HOST_UP              = 0,
        HOST_NOT_UP          = 1,
        BAD_PARAMETER        = 2,
        BAD_ADDRESS          = 3,
        NOT_ROOT             = 4,
        SOCKET_FAILED        = 5,
        NONBLOCKING_FAILED   = 6,
        EPOLL_FAILED         = 7,
        SEND_PROBLEM         = 8
    } ExitCode;
} SuperPingData;

struct option Options[] = {
    {"timeout", required_argument, NULL, 0},
    {"verbose", no_argument, NULL, 0},
    {"print-response-time", no_argument, NULL, 0},
    {"help", no_argument, NULL, 0}
};

enum OptionsIndex {
    TIMEOUT             = 0,
    VERBOSE             = 1,
    PRINT_RESPONSE_TIME = 2,
    HELP                = 3
};

static unsigned short CalculateInternetChecksum(unsigned short * addr, int len) {
    int i;
    unsigned short * a = addr;
    int sum = 0;

    for (i = 0; i < len; i += 2) {
        sum += *a;
        a++;
    }
    if (len & 1) {
        sum += ((0xff) & *a);
    }
    sum = (sum >> 16)   + 
          (sum & 0xffff);
    return (unsigned short)~sum;
}

void Die(void) {
    /* Close raw socket */
    if (SuperPingData.RawSocket >= 0) {
        close(SuperPingData.RawSocket);
        SuperPingData.RawSocket = -1;
    }
    /* Close epoll */
    if (SuperPingData.EpollFd >= 0) {
        close(SuperPingData.EpollFd);
        SuperPingData.EpollFd = -1;
    }
    /* Exit and tell how it went */
    exit(SuperPingData.ExitCode);
}

int CreateRawSocket(void) {
    int s, val;

    /* Create raw socket */
    s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s == -1) {
        fprintf(stderr, "Could not create socket.\n");
        SuperPingData.ExitCode = SOCKET_FAILED;
        Die();
    }
    /* Make socket non blocking */
    val = fcntl(s, F_GETFL);
    if (val == -1 || fcntl(s, F_SETFL, val | O_NONBLOCK) == -1) {
        fprintf(stderr, "Could not set non blocking mode.\n");
        SuperPingData.ExitCode = NONBLOCKING_FAILED;
        close(s);
        Die();
    }
    return s;
}

int StringToAddress(char * str, struct sockaddr_in * addr) {
    struct hostent * host;
    int result = 0;
    if (inet_aton(str, &addr->sin_addr) == 0) {
        if ((host = gethostbyname(str)) != NULL) {
            memcpy(&addr->sin_addr, host->h_addr, sizeof(struct in_addr));
        } else {
            /* Indicate error */
            result = -1;
        }
    }
    return result;
}

void AddRawSocketToEpoll(void) {
    int s;
    struct epoll_event event;
    s = epoll_create(1);
    SuperPingData.EpollFd = s;
    if (s == -1) {
        fprintf(stderr, "Could not create epoll file descriptor.\n");
        SuperPingData.ExitCode = EPOLL_FAILED;
        Die();
    }
    event.data.fd = SuperPingData.RawSocket;
    event.events = EPOLLIN;
    if (epoll_ctl(s, EPOLL_CTL_ADD, SuperPingData.RawSocket, &event) == -1) {
        fprintf(stderr, "Could not add socket to epoll file descriptor.\n");
        SuperPingData.ExitCode = EPOLL_FAILED;
        Die();
    }
}

int TimevalSubtract(struct timeval * result, struct timeval * x, struct timeval * y) {
    if (x->tv_usec < y->tv_usec) {
        int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
        y->tv_usec -= 1000000 * nsec;
        y->tv_sec += nsec;
    }
    if (x->tv_usec - y->tv_usec > 1000000) {
        int nsec = (x->tv_usec - y->tv_usec) / 1000000;
        y->tv_usec += 1000000 * nsec;
        y->tv_sec -= nsec;
    }

    result->tv_sec = x->tv_sec - y->tv_sec;
    result->tv_usec = x->tv_usec - y->tv_usec;

    return x->tv_sec < y->tv_sec;
}

void WaitForReply(void) {
    socklen_t len = sizeof(struct sockaddr_in);
    struct epoll_event event;
    struct sockaddr_in sender;
    int ret;
    struct iphdr * recv_ip = (struct iphdr*)SuperPingData.ReadBuffer;
    struct icmphdr * recv_icmp = (struct icmphdr *)(SuperPingData.ReadBuffer + sizeof(struct iphdr));
    struct icmphdr * send_icmp = (struct icmphdr *)SuperPingData.WriteBuffer;
    void * recv_data = recv_icmp + sizeof(struct icmphdr);
    void * send_data = send_icmp + sizeof(struct icmphdr);
    struct timeval recv_time;
    struct timeval reply_time;
    int wait_time = SuperPingData.WaitTime;

    while (1) {
        ret = epoll_wait(SuperPingData.EpollFd, &event, 1, wait_time);
        gettimeofday(&recv_time, NULL);
        TimevalSubtract(&reply_time, &recv_time, &SuperPingData.RequestTime);
        if (ret < 0) {
            fprintf(stderr, "Error waiting for reply.\n");
            SuperPingData.ExitCode = EPOLL_FAILED;
            Die();
        } else if (ret > 0 && recvfrom(SuperPingData.RawSocket, SuperPingData.ReadBuffer, READ_BUFFER_SIZE, 0, (struct sockaddr*)&sender, &len) == READ_BUFFER_SIZE) {
            /* Check if this is our reply */
            if (recv_ip->saddr == SuperPingData.AddressToPing.sin_addr.s_addr && /* Correct source address */
                    recv_icmp->type == ICMP_ECHOREPLY && /* Correct ICMP type */
                    recv_icmp->un.echo.id == send_icmp->un.echo.id && /* Correct ID */
                    recv_icmp->un.echo.sequence == send_icmp->un.echo.sequence && /* Correct sequence number */
                    memcmp(recv_data, send_data, sizeof(struct timeval)) == 0 /* Correct data */
               ) {
                /* This was a reply to our request */
                if (SuperPingData.Verbose) {
                    TimevalSubtract(&reply_time, &recv_time, &SuperPingData.RequestTime);
                    printf("Got reply in %ld seconds, %ld milliseconds and %ld microseconds.\n", reply_time.tv_sec, (reply_time.tv_usec / 1000), (reply_time.tv_usec % 1000));
                }
                if (SuperPingData.PrintResponseTime) {
                    printf("%ld\n", reply_time.tv_sec * 1000000 + reply_time.tv_usec);
                }
                SuperPingData.ExitCode = HOST_UP;
                Die();
            } else {
                /* We got something that was not a reply to our request. */
                /* Calculate a new wait time based on original wait time */
                /* and elapsed time.                                     */
                int milliseconds = (reply_time.tv_sec * 1000 + reply_time.tv_usec / 1000);
                wait_time = SuperPingData.WaitTime - milliseconds;
            }
        } else {
            SuperPingData.ExitCode = HOST_NOT_UP;
            Die();
        }
    }
}

void SuperPing(void) {
    socklen_t len = sizeof(struct sockaddr_in);
    struct icmphdr * icmp = (struct icmphdr *)SuperPingData.WriteBuffer;

    /* Initialize ICMP header */
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = getpid();
    icmp->un.echo.sequence = 0;

    /* Set data part to current time of day */
    gettimeofday(&SuperPingData.RequestTime, NULL);
    memcpy(ICMP_DATA(SuperPingData.WriteBuffer), &SuperPingData.RequestTime, sizeof(struct timeval));
    icmp->checksum = CalculateInternetChecksum((unsigned short *)SuperPingData.WriteBuffer, WRITE_BUFFER_SIZE);

    if (SuperPingData.Verbose) {
        printf("Pinging %s\n", inet_ntoa(SuperPingData.AddressToPing.sin_addr));
    }
    /* Send packet */
    if (sendto(SuperPingData.RawSocket, SuperPingData.WriteBuffer, WRITE_BUFFER_SIZE, 0, (struct sockaddr*)&SuperPingData.AddressToPing, len) != WRITE_BUFFER_SIZE) {
        fprintf(stderr, "Error sending ICMP echo request.\n");
        SuperPingData.ExitCode = SEND_PROBLEM;
        Die();
    }

    WaitForReply();
}

void CheckUser(void) {
    if (getuid() != 0) {
        fprintf(stderr, "You are not root.\n");
        SuperPingData.ExitCode = NOT_ROOT;
        Die();
    }
}

int IsNumeric(char * str) {
    while (*str) {
        if (*str < '0' || *str > '9') {
            return 0;
        }
        str++;
    }
    return 1;
}

void WriteHelp(char * prog) {
    printf(
            "Usage: %s [OPTIONS] HOST\n"
            "-h, --help                This help\n"
            "-v, --verbose             Write out what is happening\n"
            "-p, --print-response-time Write response time in nano seconds\n"
            "-t <ms>, --timeout=<ms>   Specify the maximum number of ms. to wait for a reply.\n\n"
            "SuperPing uses exit codes to indicate errors or success:\n"
            "Host is up:                      %d\n"
            "Host is not up:                  %d\n"
            "Bad parameters:                  %d\n"
            "Bad IP or host name:             %d\n"
            "Not root user:                   %d\n"
            "Failure creating socket:         %d\n"
            "Failure configuring nonblocking: %d\n"
            "Failure configuring epoll:       %d\n"
            "Problem sending packet:          %d\n"
            , prog
            , HOST_UP           
            , HOST_NOT_UP       
            , BAD_PARAMETER     
            , BAD_ADDRESS       
            , NOT_ROOT          
            , SOCKET_FAILED     
            , NONBLOCKING_FAILED
            , EPOLL_FAILED      
            , SEND_PROBLEM
          );
}

int main (int argc, char ** argv) {
    int opt, index = 0;
    /* Initialize global data */
    memset(&SuperPingData, 0, sizeof(SuperPingData));
    SuperPingData.RawSocket = -1;
    SuperPingData.EpollFd = -1;
    SuperPingData.Verbose = 0;
    SuperPingData.WaitTime = 1000; /* Wait one second */

    /* Parse command line options */
    while (1) {
        opt = getopt_long(argc, argv, "hpvt:", Options, &index);
        if (opt == -1)break;
        switch (opt) {
            case 0:
                switch (index) {
                    case TIMEOUT:
                        if (!IsNumeric(optarg)) {
                            fprintf(stderr, "Timeout argument should be numeric: %s\n", optarg);
                            SuperPingData.ExitCode = BAD_PARAMETER;
                            Die();
                        }
                        SuperPingData.WaitTime = atoi(optarg);
                        break;
                    case VERBOSE:
                        SuperPingData.Verbose = 1;
                        break;
                    case PRINT_RESPONSE_TIME:
                        SuperPingData.PrintResponseTime = 1;
                        break;
                    case HELP:
                        WriteHelp(argv[0]);
                        Die();
                        break;
                }
                break;
            case 't':
                if (!IsNumeric(optarg)) {
                    fprintf(stderr, "Timeout argument should be numeric: %s\n", optarg);
                    SuperPingData.ExitCode = BAD_PARAMETER;
                    Die();
                }
                SuperPingData.WaitTime = atoi(optarg);
                break;
            case 'v':
                SuperPingData.Verbose = 1;
                break;
            case 'p':
                SuperPingData.PrintResponseTime = 1;
                break;
            case 'h':
                WriteHelp(argv[0]);
                Die();
                break;
            case '?':
                break;
        }
    }

    if (argc == 1 || index == argc) {
        fprintf(stderr, "No address to ping specified.\n");
        WriteHelp(argv[0]);
        SuperPingData.ExitCode = BAD_PARAMETER;
        Die();
    }

    if (StringToAddress(argv[optind], &SuperPingData.AddressToPing) == -1) {
        fprintf(stderr, "Bad IP address or host name: %s\n", argv[optind]);
        SuperPingData.ExitCode = BAD_ADDRESS;
        Die();
    }

    CheckUser();

    SuperPingData.RawSocket = CreateRawSocket();

    AddRawSocketToEpoll();

    /* Send a ping */
    SuperPing();

    return 0;
}
