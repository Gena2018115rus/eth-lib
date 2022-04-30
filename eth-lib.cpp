#include "eth-lib.hpp"
#include <arpa/inet.h>

listener_t::listener_t(unsigned short port) :
//   m_port(port),
  m_sockfd{} {
    struct addrinfo *servinfo;
    const struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    
    int r = getaddrinfo(0, std::to_string(port).data(), &hints, &servinfo);
    if (r != 0)
    {
        std::cerr << "getaddrinfo() error " << r << '.' << std::endl;
        exit(-1);
    }
    if (!servinfo)
    {
        std::cerr << "addrinfo list is empty." << std::endl;
        exit(-2);
    }
    
    for (struct addrinfo *p = servinfo; p; p = p->ai_next)
    {
        m_sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            // TODO: close(sockfd)
        if (m_sockfd == -1) continue;

        setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &INT_ONE, sizeof(INT_ONE)); // не отбрасывать возвращённое значение

        if (bind(m_sockfd, p->ai_addr, p->ai_addrlen) != 0)
        {
            close(m_sockfd);
            m_sockfd = -1;
            continue;
        }

        break;
    }
    if (m_sockfd == -1)
    {
        std::cerr << "socket() or bind() error." << std::endl;
        std::cerr << "Failed to find address to bind." << std::endl;
        exit(-3);
    }

    freeaddrinfo(servinfo);

    if (listen(m_sockfd, 10000) != 0)
    {
        std::cerr << "listen() error." << std::endl;
        exit(-4);
    }

    m_sockfds.emplace_back(m_sockfd, POLLIN, 0);
}

listener_t::~listener_t() {
    close(m_sockfd);
}

client_ref_t::client_ref_t(listener_t &listener, int sockfd) :
  m_listener(listener),
  m_sockfd(sockfd) {
    
}

bool client_ref_t::write(std::string buf) {
    if (::write(m_sockfd, buf.data(), buf.size()) == -1) {
        if (errno == EPIPE)
        {
//                        to_remove_csfds.push_back(client_sockfd); // сообщать о выходе человека
            std::cerr << "write(" << m_sockfd << ") => EPIPE -- странно??" << std::endl;
        }
        else
        {
            std::cerr << "write(" << m_sockfd << ") error." << std::endl;
        }
        errno = 0;
        return false;
    }
    return true;
}

const std::string &client_ref_t::input_buf() {
    return m_listener.m_client_in_bufs.at(m_sockfd);
}

void listener_t::disconnect(int client_fd) {
    // т.к. инвалидируются итераторы, после этого вызова нельзя продолжать итерации, т.е. нужен break
    m_sockfds.erase(std::find_if(m_sockfds.begin(), m_sockfds.end(), [client_fd](auto pollfd) { return pollfd.fd == client_fd; })); // инвалидируются итераторы!
//                    send_to_all(addr_in_2str((struct sockaddr_in *)&client_addrs[fd].data) + " disconnected!");
    m_client_addrs.erase(client_fd);
    m_client_in_bufs.erase(client_fd);
    m_must_break = true;
}

void client_ref_t::disconnect() {
    m_listener.disconnect(m_sockfd);
    close(m_sockfd);
}

std::string client_ref_t::addr() {
    auto addr_in = (struct sockaddr_in *)&m_listener.m_client_addrs.at(m_sockfd).data;
    return std::string(inet_ntoa(addr_in->sin_addr)) + ':' + std::to_string(addr_in->sin_port);
}

void listener_t::send_to_all(std::string message) {
    for (auto client_it = m_sockfds.begin() + 1; client_it != m_sockfds.end(); ++client_it) {
        client_ref_t(*this, client_it->fd).write(message);
    }
}
