/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include <syslog.h>

#include <limits>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event2/listener.h>

#include <nghttp2/nghttp2.h>

#include "shrpx_config.h"
#include "shrpx_listen_handler.h"
#include "shrpx_ssl.h"

namespace shrpx {

namespace {
void ssl_acceptcb(evconnlistener *listener, int fd,
                  sockaddr *addr, int addrlen, void *arg)
{
  auto handler = reinterpret_cast<ListenHandler*>(arg);
  handler->accept_connection(fd, addr, addrlen);
}
} // namespace

namespace {
bool is_ipv6_numeric_addr(const char *host)
{
  uint8_t dst[16];
  return inet_pton(AF_INET6, host, dst) == 1;
}
} // namespace

namespace {
int resolve_hostname(sockaddr_union *addr, size_t *addrlen,
                     const char *hostname, uint16_t port, int family)
{
  addrinfo hints;
  int rv;
  char service[10];

  snprintf(service, sizeof(service), "%u", port);
  memset(&hints, 0, sizeof(addrinfo));

  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif // AI_ADDRCONFIG
  addrinfo *res;

  rv = getaddrinfo(hostname, service, &hints, &res);
  if(rv != 0) {
    LOG(FATAL) << "Unable to resolve address for " << hostname
               << ": " << gai_strerror(rv);
    return -1;
  }

  char host[NI_MAXHOST];
  rv = getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof(host),
                  0, 0, NI_NUMERICHOST);
  if(rv == 0) {
    if(LOG_ENABLED(INFO)) {
      LOG(INFO) << "Address resolution for " << hostname << " succeeded: "
                << host;
    }
  } else {
    LOG(FATAL) << "Address resolution for " << hostname << " failed: "
               << gai_strerror(rv);
    return -1;
  }
  memcpy(addr, res->ai_addr, res->ai_addrlen);
  *addrlen = res->ai_addrlen;
  freeaddrinfo(res);
  return 0;
}
} // namespace

namespace {
void evlistener_errorcb(evconnlistener *listener, void *ptr)
{
  LOG(ERROR) << "Accepting incoming connection failed";
}
} // namespace

namespace {
evconnlistener* create_evlistener(ListenHandler *handler, int family)
{
  // TODO Listen both IPv4 and IPv6
  addrinfo hints;
  int fd = -1;
  int r;
  char service[10];
  snprintf(service, sizeof(service), "%u", get_config()->port);
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif // AI_ADDRCONFIG

  addrinfo *res, *rp;
  r = getaddrinfo(get_config()->host, service, &hints, &res);
  if(r != 0) {
    if(LOG_ENABLED(INFO)) {
      LOG(INFO) << "Unable to get IPv" << (family == AF_INET ? "4" : "6")
                << " address for " << get_config()->host << ": "
                << gai_strerror(r);
    }
    return NULL;
  }
  for(rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if(fd == -1) {
      continue;
    }
    int val = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                  static_cast<socklen_t>(sizeof(val))) == -1) {
      close(fd);
      continue;
    }
    evutil_make_socket_nonblocking(fd);
#ifdef IPV6_V6ONLY
    if(family == AF_INET6) {
      if(setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &val,
                    static_cast<socklen_t>(sizeof(val))) == -1) {
        close(fd);
        continue;
      }
    }
#endif // IPV6_V6ONLY
    if(bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(fd);
  }
  if(rp) {
    char host[NI_MAXHOST];
    r = getnameinfo(rp->ai_addr, rp->ai_addrlen, host, sizeof(host),
                        0, 0, NI_NUMERICHOST);
    if(r == 0) {
      if(LOG_ENABLED(INFO)) {
        LOG(INFO) << "Listening on " << host << ", port "
                  << get_config()->port;
      }
    } else {
      LOG(FATAL) << gai_strerror(r);
      DIE();
    }
  }
  freeaddrinfo(res);
  if(rp == 0) {
    if(LOG_ENABLED(INFO)) {
      LOG(INFO) << "Listening " << (family == AF_INET ? "IPv4" : "IPv6")
                << " socket failed";
    }
    return 0;
  }

  auto evlistener = evconnlistener_new
    (handler->get_evbase(),
     ssl_acceptcb,
     handler,
     LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
     get_config()->backlog,
     fd);
  evconnlistener_set_error_cb(evlistener, evlistener_errorcb);
  return evlistener;
}
} // namespace

namespace {
void drop_privileges()
{
  if(getuid() == 0 && get_config()->uid != 0) {
    if(setgid(get_config()->gid) != 0) {
      LOG(FATAL) << "Could not change gid: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
    if(setuid(get_config()->uid) != 0) {
      LOG(FATAL) << "Could not change uid: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
    if(setuid(0) != -1) {
      LOG(FATAL) << "Still have root privileges?";
      exit(EXIT_FAILURE);
    }
  }
}
} // namespace

namespace {
void save_pid()
{
  std::ofstream out(get_config()->pid_file, std::ios::binary);
  out << getpid() << "\n";
  out.close();
  if(!out) {
    LOG(ERROR) << "Could not save PID to file " << get_config()->pid_file;
    exit(EXIT_FAILURE);
  }
}
} // namespace

namespace {
int event_loop()
{
  auto evbase = event_base_new();
  if(!evbase) {
    LOG(FATAL) << "event_base_new() failed";
    exit(EXIT_FAILURE);
  }
  SSL_CTX *sv_ssl_ctx, *cl_ssl_ctx;

  if(get_config()->client_mode) {
    sv_ssl_ctx = nullptr;
    cl_ssl_ctx = get_config()->downstream_no_tls ?
      nullptr : ssl::create_ssl_client_context();
  } else {
    sv_ssl_ctx = get_config()->upstream_no_tls ?
      nullptr : get_config()->default_ssl_ctx;
    cl_ssl_ctx = get_config()->http2_bridge &&
      !get_config()->downstream_no_tls ?
      ssl::create_ssl_client_context() : nullptr;
  }

  auto listener_handler = new ListenHandler(evbase, sv_ssl_ctx, cl_ssl_ctx);
  if(get_config()->daemon) {
    if(daemon(0, 0) == -1) {
      LOG(FATAL) << "Failed to daemonize: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
  }

  if(get_config()->pid_file) {
    save_pid();
  }

  auto evlistener6 = create_evlistener(listener_handler, AF_INET6);
  auto evlistener4 = create_evlistener(listener_handler, AF_INET);
  if(!evlistener6 && !evlistener4) {
    LOG(FATAL) << "Failed to listen on address "
               << get_config()->host << ", port " << get_config()->port;
    exit(EXIT_FAILURE);
  }

  // ListenHandler loads private key, and we listen on a priveleged port.
  // After that, we drop the root privileges if needed.
  drop_privileges();

  if(get_config()->num_worker > 1) {
    listener_handler->create_worker_thread(get_config()->num_worker);
  } else if(get_config()->downstream_proto == PROTO_SPDY) {
    listener_handler->create_http2_session();
  }

  if(LOG_ENABLED(INFO)) {
    LOG(INFO) << "Entering event loop";
  }
  event_base_loop(evbase, 0);
  if(evlistener4) {
    evconnlistener_free(evlistener4);
  }
  if(evlistener6) {
    evconnlistener_free(evlistener6);
  }
  return 0;
}
} // namespace

namespace {
// Returns true if regular file or symbolic link |path| exists.
bool conf_exists(const char *path)
{
  struct stat buf;
  int rv = stat(path, &buf);
  return rv == 0 && (buf.st_mode & (S_IFREG | S_IFLNK));
}
} // namespace

namespace {
const char *DEFAULT_NPN_LIST = NGHTTP2_PROTO_VERSION_ID ","
#ifdef HAVE_SPDYLAY
  "spdy/3,spdy/2,"
#endif // HAVE_SPDYLAY
  "http/1.1";
} // namespace

namespace {
void fill_default_config()
{
  memset(mod_config(), 0, sizeof(*mod_config()));

  mod_config()->verbose = false;
  mod_config()->daemon = false;

  mod_config()->server_name = "nghttpx nghttp2/" NGHTTP2_VERSION;
  set_config_str(&mod_config()->host, "0.0.0.0");
  mod_config()->port = 3000;
  mod_config()->private_key_file = 0;
  mod_config()->private_key_passwd = 0;
  mod_config()->cert_file = 0;

  // Read timeout for SPDY upstream connection
  mod_config()->http2_upstream_read_timeout.tv_sec = 180;
  mod_config()->http2_upstream_read_timeout.tv_usec = 0;

  // Read timeout for non-SPDY upstream connection
  mod_config()->upstream_read_timeout.tv_sec = 180;
  mod_config()->upstream_read_timeout.tv_usec = 0;

  // Write timeout for SPDY/non-SPDY upstream connection
  mod_config()->upstream_write_timeout.tv_sec = 60;
  mod_config()->upstream_write_timeout.tv_usec = 0;

  // Read/Write timeouts for downstream connection
  mod_config()->downstream_read_timeout.tv_sec = 900;
  mod_config()->downstream_read_timeout.tv_usec = 0;
  mod_config()->downstream_write_timeout.tv_sec = 60;
  mod_config()->downstream_write_timeout.tv_usec = 0;

  // Timeout for pooled (idle) connections
  mod_config()->downstream_idle_read_timeout.tv_sec = 60;

  // window bits for HTTP/2.0 and SPDY upstream/downstream
  // connection. 2**16-1 = 64KiB-1, which is HTTP/2.0 default. Please
  // note that SPDY/3 default is 64KiB.
  mod_config()->http2_upstream_window_bits = 16;
  mod_config()->http2_downstream_window_bits = 16;

  mod_config()->upstream_no_tls = false;
  mod_config()->downstream_no_tls = false;

  set_config_str(&mod_config()->downstream_host, "127.0.0.1");
  mod_config()->downstream_port = 80;
  mod_config()->downstream_hostport = 0;
  mod_config()->downstream_addrlen = 0;

  mod_config()->num_worker = 1;
  mod_config()->http2_max_concurrent_streams = 100;
  mod_config()->add_x_forwarded_for = false;
  mod_config()->no_via = false;
  mod_config()->accesslog = false;
  set_config_str(&mod_config()->conf_path, "/etc/nghttpx/nghttpx.conf");
  mod_config()->syslog = false;
  mod_config()->syslog_facility = LOG_DAEMON;
  mod_config()->use_syslog = false;
  // Default accept() backlog
  mod_config()->backlog = 256;
  mod_config()->ciphers = 0;
  mod_config()->honor_cipher_order = false;
  mod_config()->http2_proxy = false;
  mod_config()->http2_bridge = false;
  mod_config()->client_proxy = false;
  mod_config()->client = false;
  mod_config()->client_mode = false;
  mod_config()->insecure = false;
  mod_config()->cacert = 0;
  mod_config()->pid_file = 0;
  mod_config()->uid = 0;
  mod_config()->gid = 0;
  mod_config()->backend_ipv4 = false;
  mod_config()->backend_ipv6 = false;
  mod_config()->tty = isatty(fileno(stderr));
  mod_config()->cert_tree = 0;
  mod_config()->downstream_http_proxy_userinfo = 0;
  mod_config()->downstream_http_proxy_host = 0;
  mod_config()->downstream_http_proxy_port = 0;
  mod_config()->downstream_http_proxy_addrlen = 0;
  mod_config()->rate_limit_cfg = nullptr;
  mod_config()->read_rate = 1024*1024;
  mod_config()->read_burst = 4*1024*1024;
  mod_config()->write_rate = 0;
  mod_config()->write_burst = 0;
  mod_config()->npn_list = nullptr;
  mod_config()->verify_client = false;
  mod_config()->verify_client_cacert = nullptr;
  mod_config()->client_private_key_file = nullptr;
  mod_config()->client_cert_file = nullptr;
}
} // namespace

namespace {
size_t get_rate_limit(size_t rate_limit)
{
  if(rate_limit == 0) {
    return EV_RATE_LIMIT_MAX;
  } else {
    return rate_limit;
  }
}
} // namespace

namespace {
void print_version(std::ostream& out)
{
  out << get_config()->server_name << std::endl;
}
} // namespace

namespace {
void print_usage(std::ostream& out)
{
  out << "Usage: nghttpx [-Dh] [-s|--client|-p] [-b <HOST,PORT>]\n"
      << "               [-f <HOST,PORT>] [-n <CORES>] [-c <NUM>] [-L <LEVEL>]\n"
      << "               [OPTIONS...] [<PRIVATE_KEY> <CERT>]\n"
      << "\n"
      << "A reverse proxy for HTTP/2.0, SPDY/HTTPS.\n"
      << std::endl;
}
} // namespace

namespace {
void print_help(std::ostream& out)
{
  print_usage(out);
  out << "Positional arguments:\n"
      << "    <PRIVATE_KEY>      Set path to server's private key. Required\n"
      << "                       unless either -p or --client is specified.\n"
      << "    <CERT>             Set path to server's certificate. Required\n"
      << "                       unless either -p or --client is specified.\n"
      << "\n"
      << "OPTIONS:\n"
      << "\n"
      << "  Connections:\n"
      << "    -b, --backend=<HOST,PORT>\n"
      << "                       Set backend host and port.\n"
      << "                       Default: '"
      << get_config()->downstream_host << ","
      << get_config()->downstream_port << "'\n"
      << "    -f, --frontend=<HOST,PORT>\n"
      << "                       Set frontend host and port.\n"
      << "                       Default: '"
      << get_config()->host << "," << get_config()->port << "'\n"
      << "    --backlog=<NUM>    Set listen backlog size.\n"
      << "                       Default: "
      << get_config()->backlog << "\n"
      << "    --backend-ipv4     Resolve backend hostname to IPv4 address\n"
      << "                       only.\n"
      << "    --backend-ipv6     Resolve backend hostname to IPv6 address\n"
      << "                       only.\n"
      << "\n"
      << "  Performance:\n"
      << "    -n, --workers=<CORES>\n"
      << "                       Set the number of worker threads.\n"
      << "                       Default: "
      << get_config()->num_worker << "\n"
      << "    --read-rate=<RATE> Set maximum average read rate on frontend\n"
      << "                       connection. Setting 0 to this option means\n"
      << "                       read rate is unlimited.\n"
      << "                       Default: "
      << get_config()->read_rate << "\n"
      << "    --read-burst=<SIZE>\n"
      << "                       Set maximum read burst size on frontend\n"
      << "                       connection. Setting 0 to this option means\n"
      << "                       read burst size is unlimited.\n"
      << "                       Default: "
      << get_config()->read_burst << "\n"
      << "    --write-rate=<RATE>\n"
      << "                       Set maximum average write rate on frontend\n"
      << "                       connection. Setting 0 to this option means\n"
      << "                       write rate is unlimited.\n"
      << "                       Default: "
      << get_config()->write_rate << "\n"
      << "    --write-burst=<SIZE>\n"
      << "                       Set maximum write burst size on frontend\n"
      << "                       connection. Setting 0 to this option means\n"
      << "                       write burst size is unlimited.\n"
      << "                       Default: "
      << get_config()->write_burst << "\n"
      << "\n"
      << "  Timeout:\n"
      << "    --frontend-http2-read-timeout=<SEC>\n"
      << "                       Specify read timeout for HTTP/2.0 and SPDY frontend\n"
      << "                       connection. Default: "
      << get_config()->http2_upstream_read_timeout.tv_sec << "\n"
      << "    --frontend-read-timeout=<SEC>\n"
      << "                       Specify read timeout for HTTP/1.1 frontend\n"
      << "                       connection. Default: "
      << get_config()->upstream_read_timeout.tv_sec << "\n"
      << "    --frontend-write-timeout=<SEC>\n"
      << "                       Specify write timeout for all frontends.\n"
      << "                       connection. Default: "
      << get_config()->upstream_write_timeout.tv_sec << "\n"
      << "    --backend-read-timeout=<SEC>\n"
      << "                       Specify read timeout for backend connection.\n"
      << "                       Default: "
      << get_config()->downstream_read_timeout.tv_sec << "\n"
      << "    --backend-write-timeout=<SEC>\n"
      << "                       Specify write timeout for backend\n"
      << "                       connection. Default: "
      << get_config()->downstream_write_timeout.tv_sec << "\n"
      << "    --backend-keep-alive-timeout=<SEC>\n"
      << "                       Specify keep-alive timeout for backend\n"
      << "                       connection. Default: "
      << get_config()->downstream_idle_read_timeout.tv_sec << "\n"
      << "    --backend-http-proxy-uri=<URI>\n"
      << "                       Specify proxy URI in the form\n"
      << "                       http://[<USER>:<PASS>@]<PROXY>:<PORT>. If\n"
      << "                       a proxy requires authentication, specify\n"
      << "                       <USER> and <PASS>. Note that they must be\n"
      << "                       properly percent-encoded. This proxy is used\n"
      << "                       when the backend connection is HTTP/2.0. First,\n"
      << "                       make a CONNECT request to the proxy and\n"
      << "                       it connects to the backend on behalf of\n"
      << "                       nghttpx. This forms tunnel. After that, nghttpx\n"
      << "                       performs SSL/TLS handshake with the\n"
      << "                       downstream through the tunnel. The timeouts\n"
      << "                       when connecting and making CONNECT request\n"
      << "                       can be specified by --backend-read-timeout\n"
      << "                       and --backend-write-timeout options.\n"
      << "\n"
      << "  SSL/TLS:\n"
      << "    --ciphers=<SUITE>  Set allowed cipher list. The format of the\n"
      << "                       string is described in OpenSSL ciphers(1).\n"
      << "                       If this option is used, --honor-cipher-order\n"
      << "                       is implicitly enabled.\n"
      << "    --honor-cipher-order\n"
      << "                       Honor server cipher order, giving the\n"
      << "                       ability to mitigate BEAST attacks.\n"
      << "    -k, --insecure     When used with -p or --client, don't verify\n"
      << "                       backend server's certificate.\n"
      << "    --cacert=<PATH>    When used with -p or --client, set path to\n"
      << "                       trusted CA certificate file.\n"
      << "                       The file must be in PEM format. It can\n"
      << "                       contain multiple certificates. If the\n"
      << "                       linked OpenSSL is configured to load system\n"
      << "                       wide certificates, they are loaded\n"
      << "                       at startup regardless of this option.\n"
      << "    --private-key-passwd-file=<FILEPATH>\n"
      << "                       Path to file that contains password for the\n"
      << "                       server's private key. If none is given and\n"
      << "                       the private key is password protected it'll\n"
      << "                       be requested interactively.\n"
      << "    --subcert=<KEYPATH>:<CERTPATH>\n"
      << "                       Specify additional certificate and private\n"
      << "                       key file. nghttpx will choose certificates\n"
      << "                       based on the hostname indicated by client\n"
      << "                       using TLS SNI extension. This option can be\n"
      << "                       used multiple times.\n"
      << "    --backend-tls-sni-field=<HOST>\n"
      << "                       Explicitly set the content of the TLS SNI\n"
      << "                       extension.  This will default to the backend\n"
      << "                       HOST name.\n"
      << "    --dh-param-file=<PATH>\n"
      << "                       Path to file that contains DH parameters in\n"
      << "                       PEM format. Without this option, DHE cipher\n"
      << "                       suites are not available.\n"
      << "    --npn-list=<LIST>  Comma delimited list of NPN protocol sorted\n"
      << "                       in the order of preference. That means\n"
      << "                       most desirable protocol comes first.\n"
      << "                       The parameter must be delimited by a single\n"
      << "                       comma only and any white spaces are treated\n"
      << "                       as a part of protocol string.\n"
      << "                       Default: " << DEFAULT_NPN_LIST << "\n"
      << "    --verify-client    Require and verify client certificate.\n"
      << "    --verify-client-cacert=<PATH>\n"
      << "                       Path to file that contains CA certificates\n"
      << "                       to verify client certificate.\n"
      << "                       The file must be in PEM format. It can\n"
      << "                       contain multiple certificates.\n"
      << "    --client-private-key-file=<PATH>\n"
      << "                       Path to file that contains client private\n"
      << "                       key used in backend client authentication.\n"
      << "    --client-cert-file=<PATH>\n"
      << "                       Path to file that contains client\n"
      << "                       certificate used in backend client\n"
      << "                       authentication.\n"
      << "\n"
      << "  HTTP/2.0 and SPDY:\n"
      << "    -c, --http2-max-concurrent-streams=<NUM>\n"
      << "                       Set the maximum number of the concurrent\n"
      << "                       streams in one HTTP/2.0 and SPDY session.\n"
      << "                       Default: "
      << get_config()->http2_max_concurrent_streams << "\n"
      << "    --frontend-http2-window-bits=<N>\n"
      << "                       Sets the initial window size of HTTP/2.0 and SPDY\n"
      << "                       frontend connection to 2**<N>-1.\n"
      << "                       Default: "
      << get_config()->http2_upstream_window_bits << "\n"
      << "    --frontend-no-tls  Disable SSL/TLS on frontend connections.\n"
      << "    --backend-http2-window-bits=<N>\n"
      << "                       Sets the initial window size of HTTP/2.0 and SPDY\n"
      << "                       backend connection to 2**<N>-1.\n"
      << "                       Default: "
      << get_config()->http2_downstream_window_bits << "\n"
      << "    --backend-no-tls   Disable SSL/TLS on backend connections.\n"
      << "\n"
      << "  Mode:\n"
      << "    (default mode)     Accept HTTP/2.0, SPDY and HTTP/1.1 over\n"
      << "                       SSL/TLS. If --frontend-no-tls is used,\n"
      << "                       accept HTTP/2.0 and HTTP/1.1. The incoming\n"
      << "                       HTTP/1.1 connection can be upgraded to\n"
      << "                       HTTP/2.0 through HTTP Upgrade.\n"
      << "                       The protocol to the backend is HTTP/1.1.\n"
      << "    -s, --http2-proxy  Like default mode, but enable secure proxy mode.\n"
      << "    --http2-bridge     Like default mode, but communicate with the\n"
      << "                       backend in HTTP/2.0 over SSL/TLS. Thus the\n"
      << "                       incoming all connections are converted\n"
      << "                       to HTTP/2.0 connection and relayed to\n"
      << "                       the backend. See --backend-http-proxy-uri\n"
      << "                       option if you are behind the proxy and want\n"
      << "                       to connect to the outside HTTP/2.0 proxy.\n"
      << "    --client           Accept HTTP/2.0 and HTTP/1.1 without SSL/TLS.\n"
      << "                       The incoming HTTP/1.1 connection can be\n"
      << "                       upgraded to HTTP/2.0 connection through\n"
      << "                       HTTP Upgrade.\n"
      << "                       The protocol to the backend is HTTP/2.0.\n"
      << "                       To use nghttpx as a forward proxy, use -p\n"
      << "                       option instead.\n"
      << "    -p, --client-proxy Like --client option, but it also requires\n"
      << "                       the request path from frontend must be\n"
      << "                       an absolute URI, suitable for use as a\n"
      << "                       forward proxy.\n"
      << "\n"
      << "  Logging:\n"
      << "    -L, --log-level=<LEVEL>\n"
      << "                       Set the severity level of log output.\n"
      << "                       INFO, WARNING, ERROR and FATAL.\n"
      << "                       Default: WARNING\n"
      << "    --accesslog        Print simple accesslog to stderr.\n"
      << "    --syslog           Send log messages to syslog.\n"
      << "    --syslog-facility=<FACILITY>\n"
      << "                       Set syslog facility.\n"
      << "                       Default: "
      << str_syslog_facility(get_config()->syslog_facility) << "\n"
      << "\n"
      << "  Misc:\n"
      << "    --add-x-forwarded-for\n"
      << "                       Append X-Forwarded-For header field to the\n"
      << "                       downstream request.\n"
      << "    --no-via           Don't append to Via header field. If Via\n"
      << "                       header field is received, it is left\n"
      << "                       unaltered.\n"
      << "    -D, --daemon       Run in a background. If -D is used, the\n"
      << "                       current working directory is changed to '/'.\n"
      << "    --pid-file=<PATH>  Set path to save PID of this program.\n"
      << "    --user=<USER>      Run this program as USER. This option is\n"
      << "                       intended to be used to drop root privileges.\n"
      << "    --conf=<PATH>      Load configuration from PATH.\n"
      << "                       Default: "
      << get_config()->conf_path << "\n"
      << "    -v, --version      Print version and exit.\n"
      << "    -h, --help         Print this help and exit.\n"
      << std::endl;
}
} // namespace

int main(int argc, char **argv)
{
  Log::set_severity_level(WARNING);
  create_config();
  fill_default_config();

  std::vector<std::pair<const char*, const char*> > cmdcfgs;
  while(1) {
    int flag;
    static option long_options[] = {
      {"daemon", no_argument, nullptr, 'D'},
      {"log-level", required_argument, nullptr, 'L'},
      {"backend", required_argument, nullptr, 'b'},
      {"http2-max-concurrent-streams", required_argument, nullptr, 'c'},
      {"frontend", required_argument, nullptr, 'f'},
      {"help", no_argument, nullptr, 'h'},
      {"insecure", no_argument, nullptr, 'k'},
      {"workers", required_argument, nullptr, 'n'},
      {"client-proxy", no_argument, nullptr, 'p'},
      {"http2-proxy", no_argument, nullptr, 's'},
      {"version", no_argument, nullptr, 'v'},
      {"add-x-forwarded-for", no_argument, &flag, 1},
      {"frontend-http2-read-timeout", required_argument, &flag, 2},
      {"frontend-read-timeout", required_argument, &flag, 3},
      {"frontend-write-timeout", required_argument, &flag, 4},
      {"backend-read-timeout", required_argument, &flag, 5},
      {"backend-write-timeout", required_argument, &flag, 6},
      {"accesslog", no_argument, &flag, 7},
      {"backend-keep-alive-timeout", required_argument, &flag, 8},
      {"frontend-http2-window-bits", required_argument, &flag, 9},
      {"pid-file", required_argument, &flag, 10},
      {"user", required_argument, &flag, 11},
      {"conf", required_argument, &flag, 12},
      {"syslog", no_argument, &flag, 13},
      {"syslog-facility", required_argument, &flag, 14},
      {"backlog", required_argument, &flag, 15},
      {"ciphers", required_argument, &flag, 16},
      {"client", no_argument, &flag, 17},
      {"backend-http2-window-bits", required_argument, &flag, 18},
      {"cacert", required_argument, &flag, 19},
      {"backend-ipv4", no_argument, &flag, 20},
      {"backend-ipv6", no_argument, &flag, 21},
      {"private-key-passwd-file", required_argument, &flag, 22},
      {"no-via", no_argument, &flag, 23},
      {"subcert", required_argument, &flag, 24},
      {"http2-bridge", no_argument, &flag, 25},
      {"backend-http-proxy-uri", required_argument, &flag, 26},
      {"backend-no-tls", no_argument, &flag, 27},
      {"frontend-no-tls", no_argument, &flag, 29},
      {"backend-tls-sni-field", required_argument, &flag, 31},
      {"honor-cipher-order", no_argument, &flag, 32},
      {"dh-param-file", required_argument, &flag, 33},
      {"read-rate", required_argument, &flag, 34},
      {"read-burst", required_argument, &flag, 35},
      {"write-rate", required_argument, &flag, 36},
      {"write-burst", required_argument, &flag, 37},
      {"npn-list", required_argument, &flag, 38},
      {"verify-client", no_argument, &flag, 39},
      {"verify-client-cacert", required_argument, &flag, 40},
      {"client-private-key-file", required_argument, &flag, 41},
      {"client-cert-file", required_argument, &flag, 42},
      {nullptr, 0, nullptr, 0 }
    };

    int option_index = 0;
    int c = getopt_long(argc, argv, "DL:b:c:f:hkn:psv", long_options,
                        &option_index);
    if(c == -1) {
      break;
    }
    switch(c) {
    case 'D':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_DAEMON, "yes"));
      break;
    case 'L':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_LOG_LEVEL, optarg));
      break;
    case 'b':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND, optarg));
      break;
    case 'c':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_HTTP2_MAX_CONCURRENT_STREAMS,
                                       optarg));
      break;
    case 'f':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND, optarg));
      break;
    case 'h':
      print_help(std::cout);
      exit(EXIT_SUCCESS);
    case 'k':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_INSECURE, "yes"));
      break;
    case 'n':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_WORKERS, optarg));
      break;
    case 'p':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CLIENT_PROXY, "yes"));
      break;
    case 's':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_HTTP2_PROXY, "yes"));
      break;
    case 'v':
      print_version(std::cout);
      exit(EXIT_SUCCESS);
    case '?':
      exit(EXIT_FAILURE);
    case 0:
      switch(flag) {
      case 1:
        // --add-x-forwarded-for
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_ADD_X_FORWARDED_FOR,
                                         "yes"));
        break;
      case 2:
        // --frontend-http2-read-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND_HTTP2_READ_TIMEOUT,
                                         optarg));
        break;
      case 3:
        // --frontend-read-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND_READ_TIMEOUT,
                                         optarg));
        break;
      case 4:
        // --frontend-write-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND_WRITE_TIMEOUT,
                                         optarg));
        break;
      case 5:
        // --backend-read-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_READ_TIMEOUT,
                                         optarg));
        break;
      case 6:
        // --backend-write-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_WRITE_TIMEOUT,
                                         optarg));
        break;
      case 7:
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_ACCESSLOG, "yes"));
        break;
      case 8:
        // --backend-keep-alive-timeout
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_KEEP_ALIVE_TIMEOUT,
                                         optarg));
        break;
      case 9:
        // --frontend-http2-window-bits
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND_HTTP2_WINDOW_BITS,
                                         optarg));
        break;
      case 10:
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_PID_FILE, optarg));
        break;
      case 11:
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_USER, optarg));
        break;
      case 12:
        // --conf
        set_config_str(&mod_config()->conf_path, optarg);
        break;
      case 13:
        // --syslog
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_SYSLOG, "yes"));
        break;
      case 14:
        // --syslog-facility
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_SYSLOG_FACILITY, optarg));
        break;
      case 15:
        // --backlog
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKLOG, optarg));
        break;
      case 16:
        // --ciphers
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CIPHERS, optarg));
        break;
      case 17:
        // --client
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CLIENT, "yes"));
        break;
      case 18:
        // --backend-http2-window-bits
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_HTTP2_WINDOW_BITS,
                                         optarg));
        break;
      case 19:
        // --cacert
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CACERT, optarg));
        break;
      case 20:
        // --backend-ipv4
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_IPV4, "yes"));
        break;
      case 21:
        // --backend-ipv6
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_IPV6, "yes"));
        break;
      case 22:
        // --private-key-passwd-file
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_PRIVATE_KEY_PASSWD_FILE,
                                         optarg));
        break;
      case 23:
        // --no-via
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_NO_VIA, "yes"));
        break;
      case 24:
        // --subcert
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_SUBCERT, optarg));
        break;
      case 25:
        // --http2-bridge
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_HTTP2_BRIDGE, "yes"));
        break;
      case 26:
        // --backend-http-proxy-uri
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_HTTP_PROXY_URI,
                                         optarg));
        break;
      case 27:
        // --backend-no-tls
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_NO_TLS,
                                         "yes"));
        break;
      case 29:
        // --frontend-no-tls
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_FRONTEND_NO_TLS,
                                         "yes"));
        break;
      case 31:
        // --backend-tls-sni-field
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_BACKEND_TLS_SNI_FIELD,
                                         optarg));
        break;
      case 32:
        // --honor-cipher-order
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_HONOR_CIPHER_ORDER,
                                         "yes"));
        break;
      case 33:
        // --dh-param-file
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_DH_PARAM_FILE, optarg));
        break;
      case 34:
        // --read-rate
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_READ_RATE, optarg));
        break;
      case 35:
        // --read-burst
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_READ_BURST, optarg));
        break;
      case 36:
        // --write-rate
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_WRITE_RATE, optarg));
        break;
      case 37:
        // --write-burst
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_WRITE_BURST, optarg));
        break;
      case 38:
        // --npn-list
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_NPN_LIST, optarg));
        break;
      case 39:
        // --verify-client
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_VERIFY_CLIENT, "yes"));
        break;
      case 40:
        // --verify-client-cacert
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_VERIFY_CLIENT_CACERT,
                                         optarg));
        break;
      case 41:
        // --client-private-key-file
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CLIENT_PRIVATE_KEY_FILE,
                                         optarg));
        break;
      case 42:
        // --client-cert-file
        cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CLIENT_CERT_FILE, optarg));
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
  }

  // Initialize OpenSSL before parsing options because we create
  // SSL_CTX there.
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();
  SSL_library_init();
  ssl::setup_ssl_lock();

  if(conf_exists(get_config()->conf_path)) {
    if(load_config(get_config()->conf_path) == -1) {
      LOG(FATAL) << "Failed to load configuration from "
                 << get_config()->conf_path;
      exit(EXIT_FAILURE);
    }
  }

  if(argc - optind >= 2) {
    cmdcfgs.push_back(std::make_pair(SHRPX_OPT_PRIVATE_KEY_FILE,
                                     argv[optind++]));
    cmdcfgs.push_back(std::make_pair(SHRPX_OPT_CERTIFICATE_FILE,
                                     argv[optind++]));
  }

  for(size_t i = 0, len = cmdcfgs.size(); i < len; ++i) {
    if(parse_config(cmdcfgs[i].first, cmdcfgs[i].second) == -1) {
      LOG(FATAL) << "Failed to parse command-line argument.";
      exit(EXIT_FAILURE);
    }
  }

  if(!get_config()->npn_list) {
    parse_config_npn_list(DEFAULT_NPN_LIST);
  }

  if(!get_config()->subcerts.empty()) {
    mod_config()->cert_tree = ssl::cert_lookup_tree_new();
  }

  for(auto& keycert : get_config()->subcerts) {
    auto ssl_ctx = ssl::create_ssl_context(keycert.first.c_str(),
                                           keycert.second.c_str());
    if(ssl::cert_lookup_tree_add_cert_from_file
       (get_config()->cert_tree, ssl_ctx, keycert.second.c_str()) == -1) {
      LOG(FATAL) << "Failed to add sub certificate.";
      exit(EXIT_FAILURE);
    }
  }

  if(get_config()->cert_file && get_config()->private_key_file) {
    mod_config()->default_ssl_ctx =
      ssl::create_ssl_context(get_config()->private_key_file,
                              get_config()->cert_file);
    if(get_config()->cert_tree) {
      if(ssl::cert_lookup_tree_add_cert_from_file(get_config()->cert_tree,
                                                  get_config()->default_ssl_ctx,
                                                  get_config()->cert_file)
         == -1) {
        LOG(FATAL) << "Failed to parse command-line argument.";
        exit(EXIT_FAILURE);
      }
    }
  }

  if(get_config()->backend_ipv4 && get_config()->backend_ipv6) {
    LOG(FATAL) << "--backend-ipv4 and --backend-ipv6 cannot be used at the "
               << "same time.";
    exit(EXIT_FAILURE);
  }

  if(get_config()->http2_proxy + get_config()->http2_bridge +
     get_config()->client_proxy + get_config()->client > 1) {
    LOG(FATAL) << "--http2-proxy, --http2-bridge, --client-proxy and --client "
               << "cannot be used at the same time.";
    exit(EXIT_FAILURE);
  }

  if(get_config()->client || get_config()->client_proxy) {
    mod_config()->client_mode = true;
  }

  if(get_config()->client_mode || get_config()->http2_bridge) {
    mod_config()->downstream_proto = PROTO_SPDY;
  } else {
    mod_config()->downstream_proto = PROTO_HTTP;
  }

  if(!get_config()->client_mode && !get_config()->upstream_no_tls) {
    if(!get_config()->private_key_file || !get_config()->cert_file) {
      print_usage(std::cerr);
      LOG(FATAL) << "Too few arguments";
      exit(EXIT_FAILURE);
    }
  }

  char hostport[NI_MAXHOST+16];
  bool downstream_ipv6_addr =
    is_ipv6_numeric_addr(get_config()->downstream_host);
  snprintf(hostport, sizeof(hostport), "%s%s%s:%u",
           downstream_ipv6_addr ? "[" : "",
           get_config()->downstream_host,
           downstream_ipv6_addr ? "]" : "",
           get_config()->downstream_port);
  set_config_str(&mod_config()->downstream_hostport, hostport);

  if(LOG_ENABLED(INFO)) {
    LOG(INFO) << "Resolving backend address";
  }
  if(resolve_hostname(&mod_config()->downstream_addr,
                      &mod_config()->downstream_addrlen,
                      get_config()->downstream_host,
                      get_config()->downstream_port,
                      get_config()->backend_ipv4 ? AF_INET :
                      (get_config()->backend_ipv6 ?
                       AF_INET6 : AF_UNSPEC)) == -1) {
    exit(EXIT_FAILURE);
  }

  if(get_config()->downstream_http_proxy_host) {
    if(LOG_ENABLED(INFO)) {
      LOG(INFO) << "Resolving backend http proxy address";
    }
    if(resolve_hostname(&mod_config()->downstream_http_proxy_addr,
                        &mod_config()->downstream_http_proxy_addrlen,
                        get_config()->downstream_http_proxy_host,
                        get_config()->downstream_http_proxy_port,
                        AF_UNSPEC) == -1) {
      exit(EXIT_FAILURE);
    }
  }

  if(get_config()->syslog) {
    openlog("nghttpx", LOG_NDELAY | LOG_NOWAIT | LOG_PID,
            get_config()->syslog_facility);
    mod_config()->use_syslog = true;
  }

  mod_config()->rate_limit_cfg = ev_token_bucket_cfg_new
    (get_rate_limit(get_config()->read_rate),
     get_rate_limit(get_config()->read_burst),
     get_rate_limit(get_config()->write_rate),
     get_rate_limit(get_config()->write_burst),
     nullptr);

  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, 0);

  event_loop();

  ssl::teardown_ssl_lock();

  return 0;
}

} // namespace shrpx

int main(int argc, char **argv)
{
  return shrpx::main(argc, argv);
}