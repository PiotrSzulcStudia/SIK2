#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <poll.h>
#include <time.h>

#include "err.h"

#define BUFFER_SIZE 80000
#define TITLE_LEN 80
#define CMD_LEN 20
#define TIMEOUT 5000

#define UDP_SERVER 0
#define TCP_CLIENT 1

int isnumeric(char *str);
void validate_args(int argc, char *argv[]);

int main(int argc, char *argv[])
{
        struct addrinfo addr_hints;
        struct addrinfo *addr_result;

        time_t last_stream = time(0);

        int i, rcv_len, err, fd;
        char buffer[BUFFER_SIZE];
        char command[CMD_LEN];
        char title[TITLE_LEN];
        memset(title, 0, sizeof(title));
        FILE* fp;

        validate_args(argc, argv);

        if (strcmp(argv[4], "-")) {
                fp = fopen(argv[4], "w");
                fd = fileno(fp);
        } else {
                fd = 1; // stdout
        }

        struct pollfd poll_fd[2];
        poll_fd[TCP_CLIENT].events = poll_fd[UDP_SERVER].events = POLLIN;
        poll_fd[TCP_CLIENT].revents = poll_fd[UDP_SERVER].revents = 0;

        /* UDP */
        int flags, sflags;
        struct sockaddr_in server_address;
        struct sockaddr_in client_address;

        socklen_t snda_len, rcva_len;
        ssize_t len, snd_len;

        poll_fd[UDP_SERVER].fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
        if (poll_fd[UDP_SERVER].fd < 0)
                syserr("socket");

        server_address.sin_family = AF_INET; // IPv4
        server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
        server_address.sin_port = htons(atoi(argv[5])); // default port for receiving is PORT_NUM

        // bind the socket to a concrete address
        if (bind(poll_fd[UDP_SERVER].fd, (struct sockaddr *) &server_address,
                 (socklen_t) sizeof(server_address)) < 0)
                syserr("bind");

        snda_len = (socklen_t) sizeof(client_address);


        /* TCP */
        memset(&addr_hints, 0, sizeof(struct addrinfo));
        addr_hints.ai_family = AF_INET; // IPv4
        addr_hints.ai_socktype = SOCK_STREAM;
        addr_hints.ai_protocol = IPPROTO_TCP;
        err = getaddrinfo(argv[1], argv[3], &addr_hints, &addr_result);
        if (err == EAI_SYSTEM) { // system error
                syserr("getaddrinfo: %s", gai_strerror(err));
        }
        else if (err != 0) { // other error (host not found, etc.)
                fatal("getaddrinfo: %s", gai_strerror(err));
        }

        // initialize socket according to getaddrinfo results
        poll_fd[TCP_CLIENT].fd = socket(addr_result->ai_family, addr_result->ai_socktype, addr_result->ai_protocol);
        if (poll_fd[TCP_CLIENT].fd < 0)
                syserr("socket");

        // connect socket to the server
        if (connect(poll_fd[TCP_CLIENT].fd, addr_result->ai_addr, addr_result->ai_addrlen) < 0)
                syserr("connect");

        freeaddrinfo(addr_result);

        memset(buffer, 0, sizeof(buffer));
        strcpy(buffer, "GET ");
        strcat(buffer, argv[2]);
        strcat(buffer, " HTTP/1.0\r\n");

        if ((strcmp(argv[6], "yes")) == 0) {
                strcat(buffer, "Icy-MetaData:1\r\n");
        } else if ((strcmp(argv[6], "no")) != 0) {
                exit(1);
        }

        strcat(buffer, "\r\n");
        if (write(poll_fd[TCP_CLIENT].fd, buffer, strlen(buffer)) != strlen(buffer)) {
                syserr("partial / failed write");
        }

        memset(buffer, 0, sizeof(buffer));

        int pos = 0;
        int icy_metaint, meta_length, ret;
        bool play_music = true;
        bool is_header_finished = false;
        bool reading_meta = false;
        bool meta_available = strcmp(argv[6], "yes") == 0;

        while(true) {
                for (i = 0; i < 2; i++) {
                        poll_fd[i].revents = 0;
                }
                ret = poll(poll_fd, 2, TIMEOUT);
                if (ret < 0)
                        perror("poll");
                else if (ret > 0) {

                        if (poll_fd[UDP_SERVER].revents & (POLLIN | POLLERR)) {
                                fflush(stdout);
                                rcva_len = (socklen_t) sizeof(client_address);
                                flags = 0;
                                memset(command, 0, sizeof(command));
                                len = recvfrom(poll_fd[UDP_SERVER].fd, command, sizeof(command), flags,
                                               (struct sockaddr *) &client_address, &rcva_len);
                                if (len < 0)
                                        syserr("error on datagram from client socket");

                                if (!strcmp(command, "TITLE")) {
                                        sflags = 0;
                                        snd_len = sendto(poll_fd[UDP_SERVER].fd, title, (size_t) sizeof(title), sflags,
                                                         (struct sockaddr *) &client_address, snda_len);
                                        if (snd_len < 0)
                                                syserr("error on sending datagram to client socket");
                                } else if (!strcmp(command, "PLAY")) {
                                        play_music = true;
                                } else if (!strcmp(command, "PAUSE")) {
                                        play_music = false;
                                } else if (!strcmp(command, "QUIT")) {
                                        return 0;
                                }
                        }

                        if (poll_fd[TCP_CLIENT].revents & (POLLIN | POLLERR)) {

                                rcv_len = read(poll_fd[TCP_CLIENT].fd, buffer + pos, 1);
				last_stream = time(0);
                                if (rcv_len < 0) {
                                        syserr("read");
                                } else if (rcv_len == 0) {
                                        return 0;
                                }
                                pos += rcv_len;

                                if (!is_header_finished) {
                                        char* end = strstr(buffer, "\r\n\r\n");
                                        if (end) {
                                                if (meta_available) {
                                                        char *meta = strstr(buffer, "icy-metaint:");
                                                        if (meta == NULL) {
                                                                meta_available = false;
                                                        } else {
                                                                sscanf(meta + strlen("icy-metaint:"), "%d", &icy_metaint);
                                                        }
                                                }
                                                is_header_finished = true;
                                                pos = 0;
                                                memset(buffer, 0, sizeof(buffer));
                                        }
                                }

                                else if (reading_meta && (pos == meta_length)) {
                                        char *meta = strstr(buffer, "StreamTitle='");
                                        if (meta) {
                                                meta[strcspn(meta, ";") - 1] = 0;
                                                memset(title, 0, sizeof(title));
                                                strcpy(title, meta + strlen("StreamTitle='"));
                                        }
                                        pos = 0;
                                        meta_length = 0;
                                        memset(buffer, 0, sizeof(buffer));
                                        reading_meta = false;

                                } else if (!reading_meta) {
                                        if (meta_available) {
                                                if (pos == icy_metaint + 1) {
                                                        meta_length = (int)(((unsigned char) buffer[pos - 1]) * 16);
                                                        reading_meta = meta_length > 0;
                                                        memset(buffer, 0, sizeof(buffer));
                                                        pos = 0;
                                                } else if (play_music) {
                                                        if (write(fd, buffer + pos - 1, 1) < 0)
                                                                syserr("write to file");
                                                }
                                        } else {
                                                if (play_music)
                                                        if(write(fd, buffer + pos - 1, 1) < 0)
                                                                syserr("write to file");
                                                pos %= BUFFER_SIZE;
                                        }
                                }
                        }
                }

                if (time(0) - last_stream >= 5) {
                        fprintf(stderr, "timeout");
                        exit(1);
                }

        }
}

int isnumeric(char *str)
{
        while(*str)
        {
                if(!isdigit(*str))
                        return 0;
                str++;
        }

        return 1;
}

void validate_args(int argc, char *argv[])
{
        if (argc != 7) {
                exit(1);
        } else {
                if (!isnumeric(argv[3]))
                        exit(1);
                if (!isnumeric(argv[5]))
                        exit(1);
        }
}
