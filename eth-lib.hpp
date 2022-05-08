#ifndef ETH_LIB
#define ETH_LIB

#include <string.h>

class listener_t;

class client_ref_t {
  public:
    client_ref_t(listener_t *listener, int sockfd);
    virtual bool write(const char *buf, size_t count);
    const char *input_buf(); // аккуратно при использовании не из потока сервера!
    void disconnect();
    const char *addr(); // аккуратно при использовании не из потока сервера!

  private:
    listener_t *m_listener;
    int m_sockfd;
};

class listener_t {
  public:
    listener_t() = default;
    
    // onNewClient_t  = void(*)(client_ref_t client);
    // onNewMessage_t = void(*)(const char *addr, const char *msg, size_t msg_sz);
    virtual void run(void (*onNewClient)(client_ref_t), void (*onNewMessage)(const char *, const char *, size_t)) = 0;

    virtual ~listener_t() = default;
    virtual void send_to_all(const char *buf, size_t count) = 0;
};

listener_t *make_listener(unsigned short port);

class client_t {
  public:
    client_t() = default;

    // onNewData_t = void(*)(const char *chunk, size_t sz);
    virtual void listen(void (*onNewData)(const char *, size_t)) = 0;

    virtual bool write(const char *message, size_t sz) = 0;
    virtual ~client_t() = default;
};

client_t *make_client(const char *addr, const char *port);

#endif // #ifndef ETH_LIB
