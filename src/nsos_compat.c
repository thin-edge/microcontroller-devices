/* SPDX-License-Identifier: Apache-2.0
 *
 * Compatibility shim for native_sim NSOS (Native Simulator Offloaded Sockets)
 * builds.
 *
 * Zephyr excludes getnameinfo.c when CONFIG_NET_SOCKETS_OFFLOAD is set, yet the
 * POSIX layer's getnameinfo() wrapper still references zsock_getnameinfo(),
 * which open62541 uses to log peer addresses. That leaves the symbol undefined
 * at link time for NSOS builds only. Provide it here (implementation mirrors
 * Zephyr's own subsys/net/lib/sockets/getnameinfo.c). On real hardware the
 * upstream implementation is compiled, so this file is not built there.
 */

#include <stdio.h>
#include <errno.h>
#include <zephyr/net/socket.h>

int zsock_getnameinfo(const struct net_sockaddr *addr, net_socklen_t addrlen,
		      char *host, net_socklen_t hostlen,
		      char *serv, net_socklen_t servlen, int flags)
{
	ARG_UNUSED(addrlen);
	ARG_UNUSED(flags);

	/* net_sockaddr_in and _in6 share family/address offsets. */
	const struct net_sockaddr_in6 *a = (const struct net_sockaddr_in6 *)addr;

	if (host != NULL) {
		void *res = zsock_inet_ntop(a->sin6_family, &a->sin6_addr,
					    host, hostlen);
		if (res == NULL) {
			return DNS_EAI_SYSTEM;
		}
	}

	if (serv != NULL) {
		snprintk(serv, servlen, "%hu", net_ntohs(a->sin6_port));
	}

	return 0;
}
