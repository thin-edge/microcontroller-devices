// A Cumulocity smart function that converts firmware download
// progress into a Cumulocity event.
export function onMessage(message, context) {
    const decoder = new TextDecoder();
    const data = JSON.parse(decoder.decode(message.payload));

    // Skip messages with a missing or invalid percentage.
    if (
        data.percent === null ||
        data.percent === undefined ||
        typeof data.percent !== 'number' ||
        !Number.isFinite(data.percent) ||
        data.percent < 0 ||
        data.percent > 100
    ) {
        return [];
    }

    return [{
        cumulocityType: 'event',
        payload: {
            type: 'c8y_FirmwareDownload',
            text: `Firmware download ${data.phase}: ${data.name} ${data.version} (${data.percent}%)`,
            time: message.time,

            firmwareDownload: {
                name: data.name,
                version: data.version,
                phase: data.phase,
                percent: data.percent,
                bytes: data.bytes
            }
        },
        externalSource: [{
            externalId: message.clientID,
            type: 'c8y_Serial'
        }]
    }];
}
