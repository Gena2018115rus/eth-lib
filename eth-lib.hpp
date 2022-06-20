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
#include <regex>
#include <chrono>

class listener_t;

struct client_addr_t
{
    struct sockaddr data;
    socklen_t len;
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

class listener_t {
  public:
    listener_t(unsigned short port);
    
    // onNewClient_t  = void(client_ref_t client);
    // onNewMessage_t = void(std::string addr, std::string msg);
    // onDisconnect_t = void(std::string addr);
    template <typename onNewClient_t, typename onNewMessage_t>
    void listen(onNewClient_t onNewClient, onNewMessage_t onNewMessage, void (*onDisconnect)(std::string)) {
        this->onDisconnect = onDisconnect;
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

                    client_ref_t(*this, client_sockfd).write("HTTP/1.1 200 OK\r\n");
                    
                    m_sockfds.push_back({client_sockfd, POLLIN, 0}); // надо будет наверное операции с sockfds защитить мьютексами  хотя у eth-lib же всего один поток используется, не?
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
//----------------------------------------------------------------------------
                            std::string input_buf = m_client_in_bufs[client_fd];
                            std::smatch splitted;
                            if (regex_match(input_buf, splitted, std::regex{"([^]*?\r\n)\r\n([^]*)"})) { // мб сильно сэкономит память если я буду не .str(2) делать, а только 1 захвал и .suffix()
                                auto headers = splitted.str(1);
                                std::string long_poll_addr;
                                std::smatch uid;
                                if (regex_search(headers, uid, std::regex{"Cookie: session2018115-id=(\\d+)\r\n"})) {
                                    try {
                                        long_poll_addr = client_ref_t(*this, m_uid2fd.at(uid[1])).addr();
                                    } catch (const std::out_of_range &) {
                                        goto set_cookie;
                                    }
                                } else set_cookie: {
                                    std::string uid = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                                    m_uid2fd[uid] = client_fd;
                                    client_ref_t(*this, client_fd).write("Set-Cookie: session2018115-id=" + uid + "\r\n");
                                }
                                client_ref_t c(*this, client_fd);
                                c.write("\r\n");
                                onNewClient(std::move(c));

                                std::smatch m;
                                if (regex_search(headers, m, std::regex{"Content-Length: (\\d+)\r\n"})
                                    && splitted.str(2).size() >= std::stoull(m[1])) {
                                    client_ref_t client(*this, client_fd);
                                    auto addr = client.addr();
                                    client.disconnect();

                                     auto index = input_buf.find("\r\n");
                                     bool flag = false;
                                     if (index != (size_t)-1) {
                                         std::string s = {input_buf.begin(), input_buf.begin() + index};
                                         flag = regex_search(s, std::regex{"^POST /message"});
                                     }

                                     if (flag) {
//                                    if (input_buf.starts_with("POST /message")) { // clang on androin fails here and version above doesn't work on desktop?
                                        onNewMessage(long_poll_addr.empty() ? addr : long_poll_addr, std::regex_replace(input_buf, std::regex{"[^]*?\r\n\r\n"}, "", std::regex_constants::format_first_only));
                                    } else {
                                        std::clog << input_buf << std::endl;
                                    }
                                }
                            }
//----------------------------------------------------------------------------
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
    std::vector<pollfd> m_sockfds;
    std::map<int, client_addr_t> m_client_addrs; // client_addrs[fd].len is unused?     sorted_map избыточен?
    std::map<std::string, int> m_uid2fd;
    std::unordered_map<int, std::string> m_client_in_bufs;
    void (*onDisconnect)(std::string);
    bool m_must_break;
    
    friend client_ref_t;
    
    void disconnect(int client_fd);
};

class client_t {
  public:
    client_t(const std::string &addr, const std::string &port);

    // onNewData_t = void(std::string chunk);
    template <typename onNewData_t>
    void run(onNewData_t onNewData) {
        for (char in_buf[1024];;) {
            ssize_t n = read(m_sockfd, in_buf, 1024);
            if (n > 0) {
                onNewData({in_buf, in_buf + n});
            } else {
                std::clog << "disconnected from server" << std::endl;
                exit(0);
            }
        }
    }

    bool write(std::string buf);
    ~client_t();

  private:
    std::string m_addr, m_port;
    int m_sockfd;
};

#endif // #ifndef ETH_LIB
