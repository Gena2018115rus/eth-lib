#include "eth-lib.hpp"
#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <iostream>
#include <regex>

struct client_addr_t
{
    struct sockaddr data;
    socklen_t len;
};

class listener_impl_t : public listener_t {
  public:
    listener_impl_t(unsigned short port);
    
    void run(void (*onNewClient)(client_ref_t), void (*onNewMessage)(const char *, const char *, size_t)) override;
    ~listener_impl_t() override;
    void send_to_all(const char *buf, size_t count) override;
    
  private:
    // unsigned short m_port;
    int m_sockfd;
    static inline const int INT_ONE = 1;
//    std::mutex sockfds_mtx;
    std::vector<pollfd> m_sockfds;
    std::map<int, client_addr_t> m_client_addrs; // client_addrs[fd].len is unused?     sorted_map избыточен?
    std::unordered_map<int, std::string> m_client_in_bufs;
    bool m_must_break;
    
    friend client_ref_t;
    
    void disconnect(int client_fd);
};

listener_impl_t::listener_impl_t(unsigned short port) :
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

    m_sockfds.push_back({m_sockfd, POLLIN, 0});
}

listener_impl_t::~listener_impl_t() {
    close(m_sockfd);
}

client_ref_t::client_ref_t(listener_t *listener, int sockfd) :
  m_listener(listener),
  m_sockfd(sockfd) {
}

bool client_ref_t::write(const char *buf, size_t count) {
    if (::write(m_sockfd, buf, count) == -1) {
        if (errno == EPIPE)
        {
//                        to_remove_csfds.push_back(client_sockfd); // сообщать о выходе человека
            std::cerr << "write(" << m_sockfd << ") => EPIPE -- странно??" << std::endl; // сделать чтобы в файл логировалось и не трогало глобальные стримы
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

const char *client_ref_t::input_buf() {
    return dynamic_cast<listener_impl_t *>(m_listener)->m_client_in_bufs.at(m_sockfd).data();
}

void listener_impl_t::disconnect(int client_fd) {
    // т.к. инвалидируются итераторы, после этого вызова нельзя продолжать итерации, т.е. нужен break
    m_sockfds.erase(std::find_if(m_sockfds.begin(), m_sockfds.end(), [client_fd](auto pollfd) { return pollfd.fd == client_fd; })); // инвалидируются итераторы!
//                    send_to_all(addr_in_2str((struct sockaddr_in *)&client_addrs[fd].data) + " disconnected!");
    m_client_addrs.erase(client_fd);
    m_client_in_bufs.erase(client_fd);
    m_must_break = true;
}

void client_ref_t::disconnect() {
    dynamic_cast<listener_impl_t *>(m_listener)->disconnect(m_sockfd);
    close(m_sockfd);
}

const char *client_ref_t::addr() {
    static std::string save;
    
    auto addr_in = (struct sockaddr_in *)&dynamic_cast<listener_impl_t *>(m_listener)->m_client_addrs.at(m_sockfd).data;
    save = inet_ntoa(addr_in->sin_addr) + ':' + std::to_string(addr_in->sin_port);
    return save.data();
}

void listener_impl_t::send_to_all(const char *buf, size_t count) {
    for (auto client_it = m_sockfds.begin() + 1; client_it != m_sockfds.end(); ++client_it) {
        client_ref_t(this, client_it->fd).write(buf, count);
    }
}

class client_impl_t : public client_t {
  public:
    client_impl_t(const std::string &addr, const std::string &port);

    void listen(void (*onNewData)(const char *, size_t)) override;
    bool write(const char *message, size_t sz) override;
    ~client_impl_t() override;

  private:
    std::string m_addr, m_port;
    int m_sockfd;
};

client_impl_t::client_impl_t(const std::string &addr, const std::string &port) :
  m_addr(addr),
  m_port(port),
  m_sockfd(socket(AF_INET, SOCK_STREAM, 0)) {
    if (m_sockfd == -1)
    {
        std::cerr << "socket() error." << std::endl;
        exit(-5);
    }

    struct addrinfo *servinfo;
    const struct addrinfo hints = {
        .ai_flags = 0,
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    int r = getaddrinfo(m_addr.data(), m_port.data(), &hints, &servinfo);
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

    if (connect(m_sockfd, (struct sockaddr *)servinfo->ai_addr, sizeof(struct sockaddr_in)) != 0)
    {
        std::cerr << "connect() error." << std::endl;
        exit(-6);
    }

    freeaddrinfo(servinfo);
}

bool client_impl_t::write(const char *message, size_t sz) {
    client_impl_t tmp(m_addr, m_port);
    std::string buf = "POST /message HTTP/1.1\r\nContent-Length: " + std::to_string(sz) + "\r\n\r\n" + std::string{message, message + sz};
    return ::write(tmp.m_sockfd, buf.data(), buf.size()) == buf.size();
}

client_impl_t::~client_impl_t() {
    // close(m_sockfd);
}

void listener_impl_t::run(void (*onNewClient)(client_ref_t), void (*onNewMessage)(const char *, const char *, size_t)) {
    for (;;)
    {
        for (auto& sockfd : m_sockfds) sockfd.revents = 0; // need?
//            sockfds_mtx.lock();
        int poll_res = poll(m_sockfds.data(), m_sockfds.size(), -1 /*20*/);
//            sockfds_mtx.unlock();

        if (poll_res < 0)
        {
            std::cerr << "poll() error!" << std::endl;
            errno = 0;
        }

        if (m_sockfds[0].revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            std::cerr << "server_socket error!" << std::endl;
            exit(0);
        }
        else if (m_sockfds[0].revents & POLLIN)
        {
            struct sockaddr client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            int client_sockfd = accept(m_sockfd, &client_addr, &client_addr_len);
            if (client_sockfd < 0)
            {
                std::cerr << "accept() error." << std::endl;
//                    continue; // ? exit()
            }
            else
            {
                m_client_addrs[client_sockfd] = {client_addr, client_addr_len};
//                    send_to_all(addr_in_2str((struct sockaddr_in *)&client_addr) + " connected!");

                client_ref_t c(this, client_sockfd);
                const char buf[] = "HTTP/1.1 200 OK\r\n\r\n";
                c.write(buf, sizeof(buf) - 1);
                onNewClient(std::move(c));

                m_sockfds.push_back({client_sockfd, POLLIN, 0}); // надо будет наверное операции с sockfds защитить мьютексами
//                    close(client_sockfd);
            }
        }

        for (auto client_it = m_sockfds.begin() + 1; client_it != m_sockfds.end(); ++client_it)
        {
            int client_fd = client_it->fd;
            // auto client_addr = m_client_addrs[client_fd].data;
            if (client_it->revents & (POLLHUP | POLLERR | POLLNVAL))
            {
                disconnect(client_fd);
                break;
            }
            else if (client_it->revents & POLLIN)
            {
                m_must_break = false;
                for (char in_buf[16];;) {
                    ssize_t bytes_read = recv(client_fd, in_buf, sizeof(in_buf), MSG_DONTWAIT); // т.к. я работаю с raw data, я никогда не должен полагаться на \0
                    if (bytes_read > 0) {
                        // то добавить в буфер и считать ещё из сокета
                        m_client_in_bufs[client_fd] += {in_buf, in_buf + bytes_read};
                    } else if (bytes_read == 0) { // this is eof (т.к. у меня SOCK_STREAM (TCP))
                        disconnect(client_fd);
                        // надо close(client_fd)?     видимо да, т.к. если нет eof, я могу вызвать close(), но во время вызова может стать eof, соответственно можно close() на eof'нутый сокет делать
                        goto break_2;
                    } else /* bytes_read < 0 */ {
                        // error    or     ret_val = -1 and errno is EAGAIN or EWOULDBLOCK means that there are no data ready, try again later
                        if (errno != EWOULDBLOCK && errno != EAGAIN) {
                            std::cerr << "read(" << client_fd << ") error. errno = " << strerror(errno) << std::endl;
                        } else {
                            // there are no data ready, try again later
                            // МБ ПОМОЖЕТ В ОТВЕТЕ ПОСЛАТЬ content-length: 0 и пустую строку? чтобы сюда не заходить выполнению
                            // std::cerr << "уже считано " << std::regex_replace(client_in_bufs[client_fd], std::regex{"[^]*?\r\n\r\n"}, "", std::regex_constants::format_first_only).size() << std::endl;
                        }
                        errno = 0;
                        
                        std::string input_buf = m_client_in_bufs[client_fd];
                        std::smatch splitted;
                        if (regex_match(input_buf, splitted, std::regex{"([^]*?\r\n)\r\n([^]*)"})) { // мб сильно сэкономит память если я буду не .str(2) делать, а только 1 захвал и .suffix()
                            auto headers = splitted.str(1);
                            std::smatch m;
                            if (regex_search(headers, m, std::regex{"Content-Length: (\\d+)\r\n"})
                                && splitted.str(2).size() >= std::stoull(m[1])) {
                                client_ref_t client(this, client_fd);
                                std::string addr = client.addr();
                                client.disconnect();

                                auto index = input_buf.find("\r\n");
                                bool flag = false;
                                if (index != (size_t)-1) {
                                    std::string s = {input_buf.begin(), input_buf.begin() + index};
                                    flag = regex_search(s, std::regex{"^POST /message"});
                                }

                                if (flag) {
                                    auto msg = std::regex_replace(input_buf, std::regex{"[^]*?\r\n\r\n"}, "", std::regex_constants::format_first_only);
                                    onNewMessage(addr.data(), msg.data(), msg.size());
                                } else {
                                    std::clog << input_buf << std::endl;
                                }
                            }
                        }
                        
                        if (m_must_break) {
                            goto break_2;
                        }
                        
                        break;
                    }
                }
            }
        }
        break_2:;
    }
}

void client_impl_t::listen(void (*onNewData)(const char *, size_t)) {
    for (char in_buf[1024];;) {
        ssize_t n = read(m_sockfd, in_buf, 1024);
        if (n > 0) {
            onNewData(in_buf, n);
        } else {
            std::clog << "disconnected from server" << std::endl;
            exit(0);
        }
    }
}

listener_t *make_listener(unsigned short port) {
    return new listener_impl_t(port);
}

client_t *make_client(const char *addr, const char *port) {
    return new client_impl_t(addr, port);
}
