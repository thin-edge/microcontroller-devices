// Cumulocity Smart Function: thin-edge.io twin messages -> managed object fragments.
//
// Topic: te/device/*///twin/*
//
// tedge-zephyr publishes device state as twin messages, one per fragment:
//   te/device/<id>///twin/tedge_RemoteAccess  remote-access capacity
//   te/device/<id>///twin/tedge_Agent         client and firmware
//   te/device/<id>///twin/<set>               a parameter set (zephyr_modbus_telemetry, ...)
// The last topic segment names the fragment, and the payload is written
// under it unchanged, so the values a device reports are what the
// Parameters tab (DTM identifier = set name) shows and edits. The one rename
// is tedge_RemoteAccess -> remoteAccess, the fragment name already in use.
//
// The device republishes every twin value on each connect, so a fragment is
// never stale for longer than one reconnect. An empty payload removes the
// fragment's content (thin-edge.io's way of clearing a twin value).
const RENAME = { tedge_RemoteAccess: 'remoteAccess' };

export function onMessage(message, context) {
    const name = message.topic.split('/').pop();
    if (!name) {
        return [];
    }
    const text = new TextDecoder().decode(message.payload).trim();
    let value = null;
    if (text !== '') {
        try {
            value = JSON.parse(text);
        } catch (e) {
            value = text; // a bare string twin value
        }
    }
    const payload = {};
    payload[RENAME[name] || name] = value;
    return [{
        cumulocityType: 'managedObject',
        payload: payload,
        externalSource: [{ externalId: message.clientID, type: 'c8y_Serial' }],
    }];
}
