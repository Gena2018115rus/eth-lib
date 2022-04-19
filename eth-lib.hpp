#ifndef ETH_LIB
#define ETH_LIB

#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <iostream>

class client_ref_t;

struct client_addr_t
{
    struct sockaddr data;
    socklen_t len;
};

class listener_t {
  public:
    listener_t(unsigned short port);
    
    // onNewClient_t = void(client_ref_t client);
    // onNewData_t   = void(client_ref_t client);
    template <typename onNewClient_t, typename onNewData_t>
    void run(onNewClient_t onNewClient, onNewData_t onNewData) {
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

                    onNewClient(client_ref_t(*this, client_sockfd));

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
                        ssize_t bytes_readed = recv(client_fd, in_buf, sizeof(in_buf), MSG_DONTWAIT); // т.к. я работаю с raw data, я никогда не должен полагаться на \0
                        if (bytes_readed > 0) {
                            // то добавить в буфер и считать ещё из сокета
                            m_client_in_bufs[client_fd] += {in_buf, in_buf + bytes_readed};
                        } else if (bytes_readed == 0) { // this is eof (т.к. у меня SOCK_STREAM (TCP))
                            disconnect(client_fd);
                            // надо close(client_fd)?     видимо да, т.к. если нет eof, я могу вызвать close(), но во время вызова может стать eof, соответственно можно close() на eof'нутый сокет делать
                            goto break_2;
                        } else /* bytes_readed < 0 */ {
                            // error    or     ret_val = -1 and errno is EAGAIN or EWOULDBLOCK means that there are no data ready, try again later
                            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                                std::cerr << "read(" << client_fd << ") error. errno = " << strerror(errno) << std::endl;
                            } else {
                                // there are no data ready, try again later
                                // МБ ПОМОЖЕТ В ОТВЕТЕ ПОСЛАТЬ content-length: 0 и пустую строку? чтобы сюда не заходить выполнению
                                // std::cerr << "уже считано " << std::regex_replace(client_in_bufs[client_fd], std::regex{"[^]*?\r\n\r\n"}, "", std::regex_constants::format_first_only).size() << std::endl;
                            }
                            errno = 0;

                            onNewData(client_ref_t(*this, client_fd));
                    
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

    ~listener_t();
    void send_to_all(std::string message);
    
  private:
    // unsigned short m_port;
    int m_sockfd;
    static inline const int INT_ONE = 1;
//    std::mutex sockfds_mtx;
    std::vector<pollfd> m_sockfds{{m_sockfd, POLLIN, 0}};
    std::map<int, client_addr_t> m_client_addrs; // client_addrs[fd].len is unused?     sorted_map избыточен?
    std::unordered_map<int, std::string> m_client_in_bufs;
    bool m_must_break;
    
    friend client_ref_t;
    
    void disconnect(int client_fd);
};

class client_ref_t {
  public:
    client_ref_t(listener_t &listener, int sockfd);
    bool write(std::string buf);
    const std::string &input_buf();
    void disconnect();
    std::string addr();
    
  private:
    listener_t &m_listener;
    int m_sockfd;
};

#endif // #ifndef ETH_LIB
