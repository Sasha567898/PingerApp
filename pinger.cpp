#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>

#define PACKET_SIZE 128

struct Info
{
    int packets;
    int receivedPackets;
};

volatile sig_atomic_t out = 0;
// Function to handle the SIGINT signal
void handle_sigint(int signum) {
    out = 1;
}

// Function to calculate the ICMP checksum
unsigned short calculate_checksum(unsigned short *buffer, int size) {
    unsigned long sum = 0;
    for (; size > 1; size -= 2) {
        sum += *buffer++;
    }
    if (size == 1) {
        sum += *(unsigned char*)buffer;
    }
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (unsigned short)(~sum);
}

int hostname_to_ip(const char *hostname, char *ip_address_str, size_t ip_address_str_len) {
    struct addrinfo *result, *rp;
    struct sockaddr_in *ipv4;
    
    // Perform DNS resolution to get IP addresses associated with the hostname
    if (getaddrinfo(hostname, NULL, NULL, &result) != 0) {
        return -1; // DNS resolution failed
    }
    
    // Iterate through the list of IP addresses and return the first IPv4 address
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET) {
            ipv4 = (struct sockaddr_in *)rp->ai_addr;
            if (inet_ntop(AF_INET, &(ipv4->sin_addr), ip_address_str, ip_address_str_len) != NULL) {
                freeaddrinfo(result); 
                return 0; 
            }
        }
    }

    freeaddrinfo(result);
    return -1; 
}

// Function to handle pinging a single host
void *ping_host(void *arg) {
    
   
    const char *hostname = (const char *)arg;
    char target_ip[INET_ADDRSTRLEN];

    if (hostname_to_ip(hostname, target_ip, sizeof(target_ip)) != 0) {
        fprintf(stderr, "Failed to resolve IP address for %s\n", hostname);
        pthread_exit(NULL);
    }

    // Create a RAW socket
    int raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

    if (raw_socket == -1) {
        perror("socket");
        pthread_exit(NULL);
    }

    // Prepare the target address
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;

    if (inet_pton(AF_INET, target_ip, &target_addr.sin_addr) <= 0) {
        printf("Invalid address\n");
        close(raw_socket);
        pthread_exit(NULL);
    }
    pthread_t thread_id = pthread_self();
    uint16_t unique_id = (uint16_t)(uintptr_t)thread_id;

    // Construct ICMP target address
    struct icmphdr icmp_packet;
    icmp_packet.type = ICMP_ECHO;
    icmp_packet.code = 0;
    icmp_packet.un.echo.id = htons(unique_id);
    icmp_packet.un.echo.sequence = 0;
    icmp_packet.checksum = 0;
    icmp_packet.checksum = calculate_checksum((unsigned short*)&icmp_packet, sizeof(icmp_packet));

    struct timeval timeout;
    timeout.tv_sec = 1;  // Seconds
    timeout.tv_usec = 30000;  // Microseconds

    setsockopt(raw_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    int seq=0;
    int receivedPackets=0;

    while(out!=1) {
        seq++;
        // Record the time before sending
        struct timeval send_time;
        gettimeofday(&send_time, NULL);

        ssize_t bytes_sent = sendto(raw_socket, &icmp_packet, sizeof(icmp_packet), 0, (struct sockaddr*)&target_addr, sizeof(target_addr));

        if (bytes_sent == -1) {
            perror("sendto");
        } else {
            // Receive and process ICMP response
            struct sockaddr_in response_addr;
            socklen_t addr_len = sizeof(response_addr);
            unsigned char buffer[PACKET_SIZE];
             int bytes_received;
             struct icmphdr* response_icmp_header;
             do{
            bytes_received = recvfrom(raw_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&response_addr, &addr_len);
            response_icmp_header = (struct icmphdr*)(buffer + sizeof(struct ip)); // Offset to ICMP header
             }while(response_icmp_header->un.echo.id!=icmp_packet.un.echo.id);

             // Calculate round-trip time 
             struct timeval receive_time;
             gettimeofday(&receive_time, NULL);
            
             double rtt = (receive_time.tv_sec - send_time.tv_sec)*1000.0 + (receive_time.tv_usec - send_time.tv_usec) / 1000.0;
            if (bytes_received == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    printf("Timeout occurred.\n");
                } else {
                    perror("recvfrom");
                }
            } else {
               
                if (!(response_icmp_header->type == ICMP_ECHOREPLY)) {
                   sleep(1);
                    continue;
                } 
                // Print response information
                receivedPackets++;
                printf("Received ICMP response from %s : %d bytes, RTT=%.2fms\n", target_ip, bytes_received, rtt);
            }
          
        }

        sleep(1);
    }
     Info *result = (Info *)malloc(sizeof(Info));
    result->packets = seq;
    result->receivedPackets = receivedPackets;
    close(raw_socket);
    double lost = (100.0 - ((double)receivedPackets/(double)seq * 100.0)); 
    printf("\n------ %s ------ \n%d packets transmitted, %d received, %0.1f %% packet loss\n",hostname, result->packets, result->receivedPackets, lost);
    free(result); 
    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {

      // Install the signal handler for SIGINT
   signal(SIGINT, handle_sigint);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <hostname1> <hostname2> ...\n", argv[0]);
        return 1;
    }

    pthread_t threads[argc - 1];
    for (int i = 1; i < argc; i++) {
        if (pthread_create(&threads[i - 1], NULL, ping_host, (void *)argv[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (pthread_join(threads[i-1], NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
    }

    return 0;
}
