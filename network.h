#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netdb.h>
#include "protocol.h"
#include "logger.h"

// Dynamically gets local network IP address
static inline void get_local_ip(char *ip_buffer, size_t max_len)
{
    struct ifaddrs *ifaddr, *ifa;
    int family;

    if (getifaddrs(&ifaddr) == -1)
    {
        LOG_ERROR("Failed to fetch interface addresses via getifaddrs()");
        strncpy(ip_buffer, "127.0.0.1", max_len);
        return;
    }

    // Traverse linked list of interfaces looking for a valid IPv4 address that isn't loopback
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (ifa->ifa_addr == NULL)
        {
            continue;
        }
        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET)
        {
            char host[NI_MAXHOST];
            if (getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST) == 0)
            {
                if (strcmp(host, "127.0.0.1") != 0)
                {
                    strncpy(ip_buffer, host, max_len);
                    freeifaddrs(ifaddr);
                    return;
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    strncpy(ip_buffer, "127.0.0.1", max_len); // Fallback if no network interface is active
}

// Create, configure, bind, and listen on a target server port
static inline int create_listening_socket(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        LOG_ERROR("Socket creation failed on port %d", port);
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        LOG_ERROR("Setsockopt SO_REUSEADDR failed on port %d", port);
        close(server_fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to all interfaces dynamically
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        LOG_ERROR("Binding failed on port %d. Port might already be in use.", port);
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 10) < 0)
    {
        LOG_ERROR("Listen failed on socket linked to port %d", port);
        close(server_fd);
        return -1;
    }

    LOG_SUCCESS("Socket successfully listening on port %d", port);
    return server_fd;
}

// Connect to a remote target server using an explicit IP and Port destination
static inline int connect_to_server(const char *ip, int port)
{
    int sock_fd;
    struct sockaddr_in serv_addr;

    if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        LOG_ERROR("Failed to allocate client socket descriptor for destination %s:%d", ip, port);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0)
    {
        LOG_ERROR("Invalid or unsupported IP address string provided: %s", ip);
        close(sock_fd);
        return -1;
    }

    if (connect(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        LOG_ERROR("Network connection failed to target node at %s:%d", ip, port);
        close(sock_fd);
        return -1;
    }

    LOG_SUCCESS("Connected successfully to %s:%d", ip, port);
    return sock_fd;
}

#endif // NETWORK_H