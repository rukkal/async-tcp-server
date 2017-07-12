// single-threaded server based on epoll

#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <exception>
#include <array>
#include <cstring>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>


void make_socket_non_blocking(int socket_fd) {
    int flags = fcntl(socket_fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    fcntl(socket_fd, F_SETFL, flags);
}


int create_server_socket(char *port)
{
    struct addrinfo hints, *res;

    std::memset(&hints, 0, sizeof (struct addrinfo));
    hints.ai_family = AF_INET;  // IPV4
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // required for server socket

    if (getaddrinfo(NULL, port, &hints, &res) != 0)
    {
        std::perror("getaddrinfo");
        throw std::runtime_error("call to getaddrinfo failed");
    }

    int server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd == -1) {
        std::perror("socket");
        throw std::runtime_error("call to socket failed");
    }

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) == -1)
    {
        std::perror("bind");
        throw std::runtime_error("call to bind failed");
    }

    freeaddrinfo(res);
    make_socket_non_blocking(server_fd);

    return server_fd;
}


void accept_new_connections(int efd, int server_fd) {
    while(true) {
        // create new socket fd from pending listening socket queue
        struct sockaddr in_addr;
        socklen_t in_len = sizeof(in_addr);
        int infd = accept(server_fd, &in_addr, &in_len);

        if (infd == -1) {
            if( errno == EAGAIN ||
                errno == EWOULDBLOCK) {
                break; // no more incoming connections
            }
            else {
                std::perror("accept");
                break;
            }
        }

        // set socket for port
        int optval = 1;
        setsockopt(infd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

        // get the client's IP addr and port num
        char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        auto error_code = getnameinfo(  &in_addr, in_len,
                                        hbuf, sizeof(hbuf),
                                        sbuf, sizeof(sbuf),
                                        NI_NUMERICHOST | NI_NUMERICSERV);
        if (error_code == 0)
        {
            std::cout << "accepted incoming connection (host=" << hbuf << ", port=" << sbuf << ", fd=" << infd << ")" << std::endl;
        }

        // make incoming socket non-blocking
        int flags = fcntl (infd, F_GETFL, 0);
        flags |= O_NONBLOCK;
        fcntl (infd, F_SETFL, flags);

        // add incoming socket to epoll's list of file descriptors to be monitored
        struct epoll_event event;
        event.data.fd = infd;
        event.events = EPOLLIN | EPOLLET; // read operations + edge triggered mode
        auto status = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &event);
        if (status == -1)
        {
            std::perror("epoll_ctl");
            throw std::runtime_error("call to epoll_ctl failed (attempted to add new incoming connection)");
        }
    }
}

void read_input_data(int client_socket_fd) {
    bool connection_must_be_closed = false;

    while(true) {
        ssize_t count;
        char buf[512];
        count = read(client_socket_fd, buf, sizeof(buf));

        // read operation not successfull
        if(count == -1) {
            // error on reading
            if (errno != EAGAIN) {
                std::perror ("read");
                connection_must_be_closed = true;
            }
            break;
        }
        // no more data, i.e. the client has closed the connection
        else if (count == 0) {
            connection_must_be_closed = true;
            break;
        }

        buf[count]=0;
        std::cout << "incoming data (fd=" << client_socket_fd << "):" << std::string(buf) << std::endl;
    }

    if(connection_must_be_closed)
    {
        std::cout << "Closed connection on descriptor " << client_socket_fd << std::endl;

        // upon closure epoll will automatically remove the socket from the set of monitored fd
        close(client_socket_fd);
    }
}


void event_loop(int server_fd, int efd) {
    constexpr int MAX_EVENTS = 128;
    auto events_buffer = std::array<struct epoll_event, MAX_EVENTS>{};

    while(true) {
        auto new_events_count = epoll_wait(efd, events_buffer.data(), MAX_EVENTS, -1);

        for(int i = 0; i < new_events_count; i++) {
            auto& event = events_buffer[i];

            if( (event.events & EPOLLERR) ||
                (event.events & EPOLLHUP) ||
                (!(event.events & EPOLLIN))) {
                // error occured on fd or socket not ready for reading (spurious wakeups???)
                perror("epoll error");

                // upon closure epoll will automatically remove the socket from the set of monitored fd
                close (event.data.fd);
                continue;
            }
            else if (server_fd == event.data.fd) {
                accept_new_connections(efd, server_fd);
                continue;
            }
            else {
                read_input_data(event.data.fd);
            }
        }
    }
}


int main(int argc, char *argv[])
{
    if(argc != 2) {
        std::cerr << "usage: " << argv[0] << " [port]" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    int server_fd = create_server_socket(argv[1]);

    // set backlog value to the maximum
    // (number of incoming connections waiting on this socket)
    if(listen(server_fd, SOMAXCONN) == -1)
    {
        std::perror("listen");
        throw std::runtime_error("call to listen failed");
    }

    // create epoll fd
    int efd = epoll_create1(0);
    if(efd == -1)
    {
        std::perror("epoll_create1");
        throw std::runtime_error("call to epoll_create1 failed");
    }

    // add server socket to epoll's list of monitored fd
    struct epoll_event event;
    event.data.fd = server_fd;
    event.events = EPOLLIN | EPOLLET;  // read operations + edge triggered mode
    if(epoll_ctl(efd, EPOLL_CTL_ADD, server_fd, &event) == -1)
    {
        std::perror("epoll_ctl");
        throw std::runtime_error("call to epoll_ctl failed (attempted to add fd)");
    }

    // run the server
    event_loop(server_fd, efd);

    // bye...
    close(server_fd);

    return EXIT_SUCCESS;
}
