/* TCP's own options. Accepted, and nothing changes: segments go out as soon
 * as they are written already (there is no Nagle delay to turn off). */
#ifndef NETINET_TCP_H
#define NETINET_TCP_H

#define TCP_NODELAY   1
#define TCP_MAXSEG    2
#define TCP_KEEPIDLE  4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT   6

#endif
