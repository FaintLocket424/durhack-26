#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
  // Create socket
  int clientSocket = socket(AF_INET, SOCK_STREAM, 0);

  sockaddr_in serverAddress{};
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_port = htons(8080);
  
  inet_pton(AF_INET, "172.20.10.2", &serverAddress.sin_addr);;

  if (connect(clientSocket, (struct sockaddr*)&serverAddress,
            sizeof(serverAddress)) < 0) {
    perror("connect");
    return 1;
}
  std::cout << "Connected to laptop!" << std::endl;

  const char* message = "Hello from arduino";
  send(clientSocket, message, strlen(message), 0);

  std::cout << "Message sent!" << std::endl;

  close(clientSocket);
}