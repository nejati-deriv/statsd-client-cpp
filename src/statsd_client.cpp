#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <random>
#include "statsd_client.h"


/* platform-specific headers */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define CLOSE_SOCKET(s) closesocket(s)
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netdb.h>  /* Needed for getaddrinfo() and freeaddrinfo() */
    #include <unistd.h> /* Needed for close() */

    #define CLOSE_SOCKET(s) close(s)
#endif

using namespace std;
namespace statsd {

Event::Event(std::string title, std::string text, Event::Type type, tags_t tags)
    : title_{title}
    , text_{text}
    , type_{type}
    , tags_{tags}
{}

std::string Event::serialize() const
{
    std::string message;

    const auto e_text = [&]{
        std::string str;
        for (const char c : text_)
        {
            if (c == '\n')
                str.append("\\n");
            else
                str.push_back(c);
        }
        return str;
    }();

    message += "_e{" + std::to_string(title_.size()) + "," + std::to_string(e_text.size()) + "}:" + title_ + "|" + e_text;

	if (timestamp_ != 0)
        message += "|d:" + std::to_string(timestamp_);

    if (hostname_ != "")
        message += "|h:" + hostname_;

    if (aggregation_Key_ != "")
        message += "|k:" + aggregation_Key_;

    switch (priority_)
    {
        case Priority::low:    message += "|p:low";    break;
        case Priority::normal: message += "|p:normal"; break;
    }

    if (source_type_name_ != "")
        message += "|s:" + source_type_name_;

    switch (type_)
    {
        case Type::info:    message += "|t:info";    break;
        case Type::success: message += "|t:success"; break;
        case Type::warning: message += "|t:warning"; break;
        case Type::error:   message += "|t:error";   break;
    }

    if (!tags_.empty()) {
        message += "|#";
        for(std::size_t i = 0, size = tags_.size(); i < size; ++i) {
            message += tags_[i];
            if (i < size - 1)
                message += ",";
        }
    }

    return message;
}

inline bool fequal(float a, float b)
{
    const float epsilon = 0.0001;
    return ( fabs(a - b) < epsilon );
}

struct _StatsdClientData {
    int     sock;
    struct  sockaddr_in server;

    string  ns;
    string  host;
    short   port;
    bool    init;

    std::default_random_engine  rng_engine;
    std::uniform_real_distribution<> rng_dist;


    char    errmsg[1024];
};

inline bool should_send(_StatsdClientData* d, float sample_rate)
{
    if ( fequal(sample_rate, 1.0) )
    {
        return true;
    }

    float p = d->rng_dist(d->rng_engine);
    return sample_rate > p;
}

StatsdClient::StatsdClient(const string& host, int port, const string& ns)
{
    d = new _StatsdClientData;

    d->sock = -1;
    std::random_device rd;
    d->rng_engine = std::default_random_engine(rd () );
    d->rng_dist = std::uniform_real_distribution<>(0.0f, 1.0f);

    config(host, port, ns);
}

StatsdClient::~StatsdClient()
{
    // close socket
    if (d->sock >= 0) {
        CLOSE_SOCKET(d->sock);
        d->sock = -1;
        delete d;
        d = nullptr;
    }
}

void StatsdClient::config(const string& host, int port, const string& ns)
{
    d->ns = ns;
    d->host = host;
    d->port = port;
    d->init = false;
    if ( d->sock >= 0 ) {
        CLOSE_SOCKET(d->sock);
    }
    d->sock = -1;
}

int StatsdClient::init()
{
    if ( d->init ) return 0;

    d->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ( d->sock == -1 ) {
        snprintf(d->errmsg, sizeof(d->errmsg), "could not create socket, err=%m");
        return -1;
    }

    memset(&d->server, 0, sizeof(d->server));
    d->server.sin_family = AF_INET;
    d->server.sin_port = htons(d->port);

    // host must be a domain, get it from internet
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    // looks up IPv4/IPv6 address by host name or stringized IP address
    int ret = getaddrinfo(d->host.c_str(), NULL, &hints, &result);
    if ( ret ) {
        snprintf(d->errmsg, sizeof(d->errmsg),
                "getaddrinfo fail, error=%d, msg=%s", ret, gai_strerror(ret) );
        return -2;
    }
    struct sockaddr_in* host_addr = (struct sockaddr_in*)result->ai_addr;
    memcpy(&d->server.sin_addr, &host_addr->sin_addr, sizeof(struct in_addr));
    freeaddrinfo(result);

    d->init = true;
    return 0;
}

/* will change the original string */
void StatsdClient::cleanup(string& key)
{
    size_t pos = key.find_first_of(":|@");
    while ( pos != string::npos )
    {
        key[pos] = '_';
        pos = key.find_first_of(":|@");
    }
}

int StatsdClient::dec(const string& key, float sample_rate, tags_t tags)
{
    return count(key, -1, sample_rate, tags);
}

int StatsdClient::inc(const string& key, float sample_rate, tags_t tags)
{
    return count(key, 1, sample_rate, tags);
}

int StatsdClient::count(const string& key, size_t value, float sample_rate, tags_t tags)
{
    return send(key, value, "c", sample_rate, tags);
}

int StatsdClient::gauge(const string& key, size_t value, float sample_rate, tags_t tags)
{
    return send(key, value, "g", sample_rate, tags);
}

int StatsdClient::timing(const string& key, size_t ms, float sample_rate, tags_t tags)
{
    return send(key, ms, "ms", sample_rate, tags);
}

int StatsdClient::event(const Event& event)
{
    return send(event.serialize());
}

int StatsdClient::send(string key, size_t value, const string &type, float sample_rate, tags_t tags)
{
    if (!should_send(this->d, sample_rate)) {
        return 0;
    }

    cleanup(key);

    std::string tags_str;
    if (!tags.empty()) {
        tags_str.reserve(256);
        tags_str += "|#";
        for(std::size_t i = 0, size = tags.size(); i < size; ++i) {
            tags_str += tags[i];
            if (i < size -1 ) {
                tags_str += ",";
            }
        }
    }

    char buf[256];
    if ( fequal( sample_rate, 1.0 ) )
    {
        snprintf(buf, sizeof(buf), "%s%s:%zd|%s%s",
                d->ns.c_str(), key.c_str(), value, type.c_str(), tags_str.c_str());
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s%s:%zd|%s|@%.2f%s",
                d->ns.c_str(), key.c_str(), value, type.c_str(), sample_rate, tags_str.c_str());
    }

    return send(buf);
}

int StatsdClient::send(const string &message)
{
    int ret = init();
    if ( ret )
    {
        return ret;
    }
    ret = sendto(d->sock, message.data(), message.size(), 0, (struct sockaddr *) &d->server, sizeof(d->server));
    if ( ret == -1) {
        snprintf(d->errmsg, sizeof(d->errmsg),
                "sendto server fail, host=%s:%d, err=%m", d->host.c_str(), d->port);
        return -1;
    }
    return 0;
}

const char* StatsdClient::errmsg()
{
    return d->errmsg;
}

}

