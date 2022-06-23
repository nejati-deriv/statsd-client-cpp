#include <statsd_client.h>

#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include <net/if.h>
#include <netinet/in.h>
#include <string>
#include <sys/ioctl.h>
#include <vector>

static int running = 1;

void sigterm(int sig)
{
    running = 0;
}

std::string localhost()
{
    struct addrinfo hints, *info, *p;
    std::string hostname(1024, '\0');
    gethostname((char*)hostname.data(), hostname.capacity());

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; /*either IPV4 or IPV6*/
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;

    if (getaddrinfo(hostname.c_str(), "http", &hints, &info) == 0)
    {
        for (p = info; p != NULL; p = p->ai_next)
        {
            hostname = p->ai_canonname;
        }
        freeaddrinfo(info);
    }

    std::string::size_type pos = hostname.find(".");
    while (pos != std::string::npos)
    {
        hostname[pos] = '_';
        pos = hostname.find(".", pos);
    }
    return hostname;
}

std::vector<std::string>& StringSplitTrim(const std::string& sData, const std::string& sDelim, std::vector<std::string>& vItems)
{
    vItems.clear();

    std::string::size_type bpos = 0;
    std::string::size_type epos = 0;
    std::string::size_type nlen = sDelim.size();

    while (sData.substr(epos, nlen) == sDelim)
    {
        epos += nlen;
    }
    bpos = epos;

    while ((epos = sData.find(sDelim, epos)) != std::string::npos)
    {
        vItems.push_back(sData.substr(bpos, epos - bpos));
        epos += nlen;
        while (sData.substr(epos, nlen) == sDelim)
        {
            epos += nlen;
        }
        bpos = epos;
    }

    if (bpos != sData.size())
    {
        vItems.push_back(sData.substr(bpos, sData.size() - bpos));
    }
    return vItems;
}

int main(int argc, char* argv[])
{
    FILE *net, *stat;
    struct sysinfo si;
    char line[256];
    unsigned int user, nice, sys, idle, total, busy, old_total = 0, old_busy = 0;

    if (argc != 3)
    {
        printf(
            "Usage: %s host port\n"
            "Eg:    %s 127.0.0.1 8125\n",
            argv[0],
            argv[0]);
        exit(1);
    }

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); /* will save one syscall per sleep */
    signal(SIGTERM, sigterm);

    if ((net = fopen("/proc/net/dev", "r")) == NULL)
    {
        perror("fopen");
        exit(-1);
    }

    if ((stat = fopen("/proc/stat", "r")) == NULL)
    {
        perror("fopen");
        exit(-1);
    }

    std::string ns = std::string("host.") + localhost().c_str() + ".";
    statsd::StatsdClient client(argv[1], atoi(argv[2]), ns);

    daemon(0, 0);
    printf("running in background.\n");

    while (running)
    {
        rewind(net);
        std::vector<std::string> items;
        while (!feof(net))
        {
            fgets(line, sizeof(line), net);
            StringSplitTrim(line, " ", items);

            if (items.size() < 17)
                continue;
            if (items[0].find(":") == std::string::npos)
                continue;
            if (items[1] == "0" and items[9] == "0")
                continue;

            std::string netface = "network." + items[0].erase(items[0].find(":"));
            client.count(netface + ".receive.bytes", atoll(items[1].c_str()));
            client.count(netface + ".receive.packets", atoll(items[2].c_str()));
            client.count(netface + ".transmit.bytes", atoll(items[9].c_str()));
            client.count(netface + ".transmit.packets", atoll(items[10].c_str()));
        }

        sysinfo(&si);
        client.gauge("system.load", 100 * si.loads[0] / 0x10000);
        client.gauge("system.freemem", si.freeram / 1024);
        client.gauge("system.procs", si.procs);
        client.count("system.uptime", si.uptime);

        /* rewind doesn't do the trick for /proc/stat */
        freopen("/proc/stat", "r", stat);
        fgets(line, sizeof(line), stat);
        sscanf(line, "cpu  %u %u %u %u", &user, &nice, &sys, &idle);
        total = user + sys + idle;
        busy = user + sys;

        // client.send("system.cpu", 100 * (busy - old_busy) / (total - old_total), "g", 1.0);

        old_total = total;
        old_busy = busy;
        sleep(6);
    }

    fclose(net);
    fclose(stat);

    exit(0);
}
