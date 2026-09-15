# SPDX-License-Identifier: Apache-2.0
#
# Shared, protocol-agnostic core: connectivity (Wi-Fi + mDNS/DNS-SD), optional
# status display, the device data model (measurements + writable control points)
# and device identity. Consumed by every apps/<protocol> application; contains
# no protocol-frontend code and does not depend on any protocol stack.

zephyr_library()

# Connectivity management (Wi-Fi on hardware, host backend on native_sim) and
# mDNS/DNS-SD advertisement.
zephyr_library_sources(net.c)

# NSOS (native_sim offloaded sockets) link shim — provides zsock_getnameinfo(),
# which Zephyr omits under CONFIG_NET_SOCKETS_OFFLOAD. Not built on hardware.
zephyr_library_sources_ifdef(CONFIG_NET_SOCKETS_OFFLOAD nsos_compat.c)

# Optional TFT status display (no-op when CONFIG_APP_DISPLAY_STATUS is off).
zephyr_library_sources(display.c)

# Device data model: writable control points and device/firmware identity.
zephyr_library_sources(controls.c identity.c)

# Selected simulation implementation (exactly one, chosen by the Kconfig choice).
# Both implement the data_source.h interface + app_sim_fault().
zephyr_library_sources_ifdef(CONFIG_APP_SIM_ENVIRONMENT sim_environment.c)
zephyr_library_sources_ifdef(CONFIG_APP_SIM_PUMP        sim_pump.c)

# Export the public headers (net.h, display.h, data_source.h, controls.h,
# identity.h) to every consumer of this module.
zephyr_include_directories(.)
