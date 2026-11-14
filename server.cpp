#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

int main() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(8080);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&serverAddress,
             sizeof(serverAddress)) < 0) {
        perror("bind");
        return 1;
    }

    std::cout << "Socket bound!" << std::endl;

    if (listen(serverSocket, 5) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "Listening on port 8080..." << std::endl;

    int clientSocket = accept(serverSocket, nullptr, nullptr);

    if (clientSocket < 0) {
        perror("accept");
        return 1;
    }

    std::cout << "Client connected!" << std::endl;

    char buffer[1024] = {};

    ssize_t bytes = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "Message: " << buffer << std::endl;
    }

    close(clientSocket);
    close(serverSocket);
}