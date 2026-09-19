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

# Optional connectivity status LED (self-guards on CONFIG_APP_STATUS_LED + a
# board `led0` alias; compiles to a no-op otherwise).
zephyr_library_sources(status_led.c)

# Wi-Fi provisioning support: the boot request that selects the provisioner
# image, the identity record it names itself after, and the application-side
# hand-off with its sw0 gesture classifier. The provisioner itself (Improv over
# BLE) is its own application: apps/wifi-provisioner.
zephyr_library_sources_ifdef(CONFIG_APP_BOOT_REQUEST boot_request.c)
zephyr_library_sources_ifdef(CONFIG_APP_WIFI_CRED_STORE prov_identity.c)
zephyr_library_sources_ifdef(CONFIG_APP_PROV_HANDOFF
  prov_handoff.c
  button_gesture.c
)

# Liveness watchdog and health diagnostics (both off by default; the progress
# hook app_alive() in liveness.h compiles to nothing without them).
zephyr_library_sources_ifdef(CONFIG_APP_DIAG     diag.c)
zephyr_library_sources_ifdef(CONFIG_APP_LIVENESS liveness.c)
if(CONFIG_APP_DIAG OR CONFIG_APP_LIVENESS)
  zephyr_library_sources(progress.c)
endif()

# Device data model: writable control points and device/firmware identity.
zephyr_library_sources(controls.c identity.c)

# Selected simulation implementation (exactly one, chosen by the Kconfig choice).
# Both implement the data_source.h interface + app_sim_fault().
zephyr_library_sources_ifdef(CONFIG_APP_SIM_ENVIRONMENT sim_environment.c)
zephyr_library_sources_ifdef(CONFIG_APP_SIM_PUMP        sim_pump.c)
zephyr_library_sources_ifdef(CONFIG_APP_SIM_SWITCH      sim_switch.c)

# Export the public headers (net.h, display.h, data_source.h, controls.h,
# identity.h) to every consumer of this module.
zephyr_include_directories(.)
