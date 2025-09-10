#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDP_PORT 12345

int main(int argc, char *argv[]) {
  int sockfd;
  struct sockaddr_in server_addr;
  const char *message = "Hello from UDP client!";
  const char *server_ip = (argc > 1) ? argv[1] : "127.0.0.1";

  printf("UDP Client sending to %s:%d\n", server_ip, UDP_PORT);

  // Create UDP socket
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return 1;
  }

  // Setup server address
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(UDP_PORT);
  server_addr.sin_addr.s_addr = inet_addr(server_ip);

  // Send message
  ssize_t sent = sendto(sockfd, message, strlen(message), 0,
                        (struct sockaddr *)&server_addr, sizeof(server_addr));

  if (sent < 0) {
    perror("sendto");
    close(sockfd);
    return 1;
  }

  printf("Sent UDP packet: '%s' (%zd bytes)\n", message, sent);

  close(sockfd);
  return 0;
}
