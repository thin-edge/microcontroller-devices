// Cumulocity Smart Function: tedge-zephyr events -> Cumulocity events.
//
// Topic: te/device/*///e/*
//
// The device sends {"text": "...", "time": "..."} (time only once its clock
// is set), e.g.
//   te/device/<id>///e/zephyr_Identify   the sw0 identify pattern
//   te/device/<id>///e/app_test          the `tedge event` test command
// Every type is mapped: the event type is the last topic segment, the text
// falls back to the type, and any further payload field is kept as a
// fragment.
export function onMessage(message, context) {
    const type = message.topic.split('/').pop();
    let data;
    try {
        data = JSON.parse(new TextDecoder().decode(message.payload));
    } catch (e) {
        return [];
    }
    if (data === null || typeof data !== 'object' || Array.isArray(data)) {
        return [];
    }
    // The device's own timestamp when it sent one (the runtime wants a Date).
    const time = typeof data.time === 'string' ? new Date(data.time) : message.time;
    const text = typeof data.text === 'string' && data.text !== '' ? data.text : type;
    const payload = { type: type, text: text, time: time };
    for (const [key, value] of Object.entries(data)) {
        if (key !== 'time' && key !== 'text' && key !== 'type') {
            payload[key] = value;
        }
    }
    return [{
        cumulocityType: 'event',
        payload: payload,
        externalSource: [{ externalId: message.clientID, type: 'c8y_Serial' }],
    }];
}
