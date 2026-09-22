// Cumulocity Smart Function: tedge-zephyr measurements -> Cumulocity measurements.
//
// Topic: te/device/*///m/*
//
// Payloads are flat, one number per series and no units (README, "Telemetry
// in Cumulocity"):
//   te/device/<id>///m/pump          modbus-server
//   te/device/<id>///m/server        opcua-server
//   te/device/<id>///m/agent         snmp-agent
//   te/device/<id>///m/device        tedge-agent
//   te/device/<id>///m/tedge_health  every tedge image
// Known types get a Cumulocity type, one fragment and named series with
// units. Any other type (an operator can rename the Modbus one) is kept
// rather than dropped: fragment = type, series = key, no unit.
const DEFINITIONS = {
    pump: {
        type: 'c8y_Pump', fragment: 'pump',
        series: {
            flow_lpm: ['flow', 'l/min'],
            pressure_bar: ['pressure', 'bar'],
            motor_temp_c: ['motorTemperature', 'C'],
            rpm: ['speed', 'rpm'],
            vibration_mms: ['vibration', 'mm/s'],
            run_hours: ['runHours', 'h'],
        },
    },
    server: {
        type: 'c8y_Server', fragment: 'server',
        series: {
            temperature: ['temperature', 'C'],
            humidity: ['humidity', '%RH'],
            pressure: ['pressure', 'hPa'],
        },
    },
    agent: {
        type: 'c8y_Agent', fragment: 'agent',
        series: { uptime: ['uptime', 's'] },
    },
    device: {
        type: 'c8y_Device', fragment: 'device',
        series: {
            uptime: ['uptime', 's'],
            heap_free: ['freeHeap', 'bytes'],
            rssi: ['rssi', 'dBm'],
        },
    },
    tedge_health: {
        type: 'c8y_TedgeHealth', fragment: 'tedge_Health',
        series: {
            uptime: ['uptime', 's'],
            freeHeap: ['freeHeap', 'bytes'],
            droppedMessages: ['droppedMessages', ''],
            resetCause: ['resetCause', ''],
        },
    },
};

export function onMessage(message, context) {
    const source = message.topic.split('/').pop();
    let data;
    try {
        data = JSON.parse(new TextDecoder().decode(message.payload));
    } catch (e) {
        return [];
    }
    const def = DEFINITIONS[source] ||
        { type: source, fragment: source, series: {} };
    const fragment = {};
    for (const [key, value] of Object.entries(data)) {
        if (key === 'time' || typeof value !== 'number' || !Number.isFinite(value)) {
            continue;
        }
        const [name, unit] = def.series[key] || [key, undefined];
        fragment[name] = unit === undefined ? { value: value } : { value: value, unit: unit };
    }
    if (Object.keys(fragment).length === 0) {
        return [];
    }
    // The device's own timestamp when it sent one (the runtime wants a Date).
    const time = typeof data.time === 'string' ? new Date(data.time) : message.time;
    const payload = { type: def.type, time: time };
    payload[def.fragment] = fragment;
    return [{
        cumulocityType: 'measurement',
        payload: payload,
        externalSource: [{ externalId: message.clientID, type: 'c8y_Serial' }],
    }];
}
