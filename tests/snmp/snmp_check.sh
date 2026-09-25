#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Exercise an SNMP agent the way the golden data under tests/snmp/golden/ was
# captured, and print a normalised transcript to compare with it:
#
#   tests/snmp/snmp_check.sh <host> > out.txt
#   diff tests/snmp/golden/switch-5if.txt out.txt
#
# Sections: a full snmpwalk of 1.3.6.1, a snmpbulkwalk with 25 repetitions,
# GETs and GETNEXTs of OIDs that are prefixes of, between, before and after the
# leaves, an oversized GETBULK (200 repetitions, bounded by the agent), and a
# malformed datagram followed by a GET that shows the agent still answers.
#
# Values that legitimately differ between runs or builds are replaced:
# Timeticks and Counter32 values, ifOperStatus (one simulated link flaps), and
# the strings that carry the build timestamp (sysDescr.0 and the enterprise
# firmware-build scalar). Everything else — every OID, its order, its type and
# its value — must match exactly.
#
# The golden file was captured from apps/snmp-agent built for native_sim
# (CONFIG_APP_SIM_SWITCH_IF_COUNT=5) at the commit before the MIB table was
# compacted (reduce-memory-footprint, group 3); the same script on a board
# with the same interface count must give the same transcript.
set -uo pipefail

host=${1:?usage: $0 <host>}
c=(-v2c -c public -On -t 3 -r 1 "$host")

normalise() {
	sed -E \
		-e 's/(= Timeticks: ).*/\1<volatile>/' \
		-e 's/(= Counter32: ).*/\1<volatile>/' \
		-e 's#^(\.?iso|\.?1)\.3\.6\.1\.2\.1\.2\.2\.1\.8\.([0-9]+) = INTEGER: .*#\1.3.6.1.2.1.2.2.1.8.\2 = INTEGER: <volatile>#' \
		-e 's#^(\.?iso|\.?1)\.3\.6\.1\.2\.1\.1\.1\.0 = STRING: .*#\1.3.6.1.2.1.1.1.0 = STRING: <build-dependent>#' \
		-e 's#^(\.?iso|\.?1)\.3\.6\.1\.4\.1\.99999\.1\.3\.0 = STRING: .*#\1.3.6.1.4.1.99999.1.3.0 = STRING: <build-dependent>#'
}

# OIDs that are not leaves: prefixes (the system group, sysDescr without its
# .0, ifEntry), between leaves (an ifTable column the agent does not serve,
# a row past the last interface, sysDescr.1), before the first leaf, after the
# last leaf, and a long OID.
probes=(
	1.3.6.1.2.1.1
	1.3.6.1.2.1.1.1
	1.3.6.1.2.1.1.1.1
	1.3.6.1.2.1.1.0
	1.3.6.1.2.1.2.2.1
	1.3.6.1.2.1.2.2.1.6.1
	1.3.6.1.2.1.2.2.1.1.9
	1.3.6.1.2.1.2.2.1.17.5
	1.3.6.1.2.1.2.2.1.17.6
	1.3.6.1.2.1.0
	1.3.6.1.2.1.99
	1.3.6.1.4.1.99999.1
	1.3.6.1.4.1.99999.1.3.0
	1.3.6.1.4.1.99999.1.4.0
	1.3.6.1.4.1.99999.2
	1.3.6.1.2.1.1.1.0.1.2.3.4.5.6.7.8.9.10.11.12.13.14.15.16.17.18.19.20.21.22
)

echo "## snmpwalk 1.3.6.1"
snmpwalk "${c[@]}" 1.3.6.1 2>&1 | normalise
echo "## snmpbulkwalk -Cr25 1.3.6.1"
snmpbulkwalk "${c[@]}" -Cr25 1.3.6.1 2>&1 | normalise
echo "## snmpget probes"
snmpget "${c[@]}" "${probes[@]}" 2>&1 | normalise
echo "## snmpgetnext probes"
snmpgetnext "${c[@]}" "${probes[@]}" 2>&1 | normalise
echo "## snmpbulkget -Cn0 -Cr200 1.3.6.1.2.1.2.2.1 (oversized)"
snmpbulkget "${c[@]}" -Cn0 -Cr200 1.3.6.1.2.1.2.2.1 2>&1 | normalise
echo "## snmpbulkget -Cn1 -Cr3 sysUpTime.0 + two repeaters"
snmpbulkget "${c[@]}" -Cn1 -Cr3 1.3.6.1.2.1.1.3.0 1.3.6.1.2.1.2.2.1.1 1.3.6.1.4.1.99999.1.2.0 2>&1 | normalise
echo "## malformed datagrams, then a GET"
python3 - "$host" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1)
for pkt in (b"\x30\x82\xff\xff\x02\x01\x01", b"\x30\x05\x02\x01\x01\x04\x7f",
            b"\x30\x1c\x02\x01\x01\x04\x06public\xa0\x0f\x02\x01\x01\x02\x01\x00\x02\x01\x00\x30\x84",
            bytes(range(256))):
    s.sendto(pkt, (sys.argv[1], 161))
    try:
        d, _ = s.recvfrom(1500)
        print("unexpected reply", d.hex())
    except socket.timeout:
        print("no reply (dropped)")
PY
snmpget "${c[@]}" 1.3.6.1.2.1.1.5.0 1.3.6.1.2.1.2.1.0 2>&1 | normalise
