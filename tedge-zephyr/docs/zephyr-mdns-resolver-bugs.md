# Zephyr's resolver cannot look up `.local` names on a typical device

A report prepared for upstream Zephyr (`zephyrproject-rtos/zephyr`). It is
kept here because the client works around it, and the workaround should be
removed once it is fixed.

- **Affects:** Zephyr 4.4.2 (seen), `subsys/net/lib/dns/resolve.c` and
  `dispatcher.c`
- **Severity:** `.local` lookups never succeed on a device that takes DNS
  from DHCP and also runs the mDNS responder
- **Workaround in this module:** `src/tedge_remote_access.c`
  (`mdns_once()`) sends its own one-shot mDNS query for a remote-access
  target and reads the answer itself; Zephyr's resolver is used only for
  unicast names.

Seen on an ESP32-S3-DevKitC-1, `apps/modbus-server` with the `full`
profile, 2026-10-07, with a Raspberry Pi (systemd-resolved) as the host
being looked up. Both problems below have to be fixed for a `.local`
lookup to work; the first hides the second.

## 1. With DNS from DHCP there is no mDNS server

`dns_resolve_init_default()` adds the mDNS server (224.0.0.251:5353) to
the default context only inside `#if defined(CONFIG_DNS_SERVER_IP_ADDRESSES)`.
A device that gets its DNS server from DHCP has no static servers, so the
default context never gets an mDNS server, whatever `CONFIG_MDNS_RESOLVER`
says. `dns_resolve_name_internal()` then finds no server for the `.local`
query and returns `-ENOENT`, which `getaddrinfo()` reports as
`DNS_EAI_SYSTEM` (-11).

```
uart:~$ net dns
DNS servers:
	192.168.178.1:53 via wlan0 (DHCP)
uart:~$ net dns rpi5-d83add9f145a.local
Cannot resolve 'rpi5-d83add9f145a.local' (-2)
```

Setting `CONFIG_DNS_SERVER_IP_ADDRESSES=y` with no servers adds the mDNS
server, but in **slot 0**: the empty static entry is skipped, and DHCP's
server lands in slot 1. The resolver keeps `.local` queries off unicast
servers, but not the reverse: every other name goes to the first server
with a socket. Ordinary lookups (`pool.ntp.org`) then go to
224.0.0.251:5353 and time out, and the device cannot connect. Zephyr's own
default list puts mDNS *after* the configured servers, which is why this
only shows up when that list is empty.

Adding the mDNS server after DHCP has configured its own
(`dns_resolve_reconfigure()` with `"224.0.0.251:5353"`, with
`DNS_RECONFIGURE_CLEANUP` off) keeps the order and fixes this part.

(The `NET_DBG` in the `servers_sa` branch of `dns_resolve_init_locked()`
also prints `ctx->servers[i]` where it means `ctx->servers[idx]`, so the
DHCP server is logged with the mDNS flag of slot 0. Misleading while
debugging this.)

## 2. With the mDNS responder running, the answers are never read

When the application runs `MDNS_RESPONDER`, the responder owns UDP 5353.
`register_dispatcher()` finds that entry for the resolver's mDNS socket
and **pairs** the two: the resolver socket is neither bound nor registered
with the socket service. The resolver then sends its query from that
unbound socket, so from an ephemeral port. RFC 6762 section 6.7 makes that
a "legacy unicast" query, which responders answer by unicast to the
source port; the paired design expects answers on 5353. Nothing reads the
ephemeral port, and every lookup times out.

A capture on the host being looked up shows the exchange working on the
wire:

```
192.168.68.67.39717 > 224.0.0.251.5353: 0+ A (QM)? rpi5-d83add9f145a.local. (41)
192.168.68.84.5353 > 192.168.68.67.39717: 0*- 1/0/0 A 192.168.68.84 (57)
```

and the device logs `Query timeout DNS req 0` two seconds later.

## Suggested fixes

1. Add the mDNS (and LLMNR) servers in `dns_resolve_init_default()`
   whether or not static servers are configured, after them; and skip
   mDNS servers when choosing a server for a non-`.local` name.
2. When pairing a resolver socket with a responder, either send the query
   from the responder's 5353 socket (so answers are multicast and reach
   it), or also bind and poll the resolver's own socket for unicast
   answers.

## Removing the workaround

Once both are fixed, `mdns_once()` and the packet helpers in
`src/tedge_ra_target.c` can go, and `.local` names can take the same
`dns_get_addr_info()` path as other names, with
`CONFIG_TEDGE_REMOTE_ACCESS_MDNS` selecting `MDNS_RESOLVER`.
