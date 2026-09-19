## 1. The bridge in the module

- [ ] 1.1 `tedge-zephyr/src/tedge_remote_access.c`: parse `530`, resolve the target, check the policy and the application hook, take a seat (D2, D3)
- [ ] 1.2 TCP connect to the target, then the WebSocket to `wss://<tenant>/service/remoteaccess/device/<key>` with `Sec-WebSocket-Protocol: binary` and the Bearer token; never log the key
- [ ] 1.3 The bridge loop: two static buffers, one frame per read, idle timeout, close reasons, byte counters (D4)
- [ ] 1.4 Telnet echo offer for port 23 (D5)
- [ ] 1.5 Session pool and cap: `TEDGE_REMOTE_ACCESS_MAX_SESSIONS` threads and buffers; a request over the cap fails immediately with the limit in the reason
- [ ] 1.6 Results to the client thread through a message queue; the dispatcher in `tedge_c8y.c` sends `501`/`503`/`502` and the open/close events
- [ ] 1.7 `tedge_RemoteAccess` twin data through the core's twin store, on connect and on every session change; `TEDGE_REMOTE_ACCESS_TWIN_SESSIONS` controls the target list
- [ ] 1.8 Kconfig: `TEDGE_REMOTE_ACCESS` selects `TEDGE_HTTP`, `WEBSOCKET_CLIENT` and `PSA_WANT_ALG_SHA_1`; add `TEDGE_REMOTE_ACCESS_STACK_SIZE` and `TEDGE_REMOTE_ACCESS_TWIN_SESSIONS`; drop the experimental gate

## 2. Tests

- [ ] 2.1 Unit tests (`native_sim`): `530` parsing (including a quoted key), the policy check against a subnet and an allow-list, and the twin JSON
- [ ] 2.2 A Kconfig case: the feature off leaves no WebSocket client in the image; on, it requires CA authentication
- [ ] 2.3 Check that no test or log path prints the connection key

## 3. Hardware verification

- [ ] 3.1 C6 Modbus + client: SSH to the Pi through the device; record time to first command, echo latency and throughput both ways
- [ ] 3.2 Policy: a target outside the subnet and an unreachable target on it both fail with their reasons, and nothing is dialled
- [ ] 3.3 Cap: a second session while one is open fails within seconds and the first survives
- [ ] 3.4 Twin data and events in Cumulocity across open, close and a reboot with a session open
- [ ] 3.5 Telnet target: echo in Cumulocity's web terminal
- [ ] 3.6 TLS heap and TCP contexts before, during and after a session (no leak), and the footprint row in `measure_tedge.sh`

## 4. Documentation

- [ ] 4.1 README: what the feature needs from the application (TLS contexts, heap, sockets), the policy, the cap and the throughput it gives
- [ ] 4.2 Update `full.conf` and `remote-access-enabler.conf`, and record the results in design.md
