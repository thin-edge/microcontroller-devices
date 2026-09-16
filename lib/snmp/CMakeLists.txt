# SPDX-License-Identifier: Apache-2.0
#
# SNMP agent protocol frontend: a minimal SNMPv2c agent (UDP) that maps the
# shared switch/router data model (lib/common) onto a system + interfaces MIB
# view, plus a trap sender. BER/ASN.1 and PDU handling are implemented here; no
# external SNMP stack is used.

if(CONFIG_APP_SNMP_AGENT)
  zephyr_library()
  zephyr_library_sources(snmp_ber.c snmp_mib.c snmp_agent.c)
  zephyr_library_sources_ifdef(CONFIG_APP_SNMP_TRAP snmp_trap.c)
  zephyr_include_directories(.)
endif()
