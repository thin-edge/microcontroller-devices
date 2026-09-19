#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host-side probe of the Cumulocity MQTT paths the spike firmware uses.

It connects once, as the device, and exercises what the firmware will do:
- SmartREST: publish 100 (create), 114 (supported operations) and 117
  (required interval) on s/us, and watch s/e for errors;
- s/uat -> s/dat: request a JWT (only its length and claims are printed);
- free-form: publish one thin-edge.io-shaped measurement (MQTT Service only).

Running the same checks from a PC first means a device-side failure can't be
mistaken for a tenant problem.

Credentials come from the environment, never from arguments:
  C8Y_DOMAIN, and either
  - basic auth: SPIKE_A_DEVICE_ID, SPIKE_A_MQTT_USER, SPIKE_A_MQTT_PASSWORD
  - certificate: PROBE_DEVICE_ID (= the certificate CN), PROBE_CERT_FILE,
    PROBE_KEY_FILE

Usage: c8y_mqtt_probe.py <port>   (9883 = MQTT Service, 8883 = Core MQTT)
"""

import base64
import json
import os
import ssl
import sys
import threading
import time

import paho.mqtt.client as mqtt


def jwt_summary(token: str) -> str:
    try:
        payload = token.split(".")[1]
        payload += "=" * (-len(payload) % 4)
        claims = json.loads(base64.urlsafe_b64decode(payload))
        keep = {k: claims[k] for k in ("sub", "ten", "exp", "iat") if k in claims}
        return f"len={len(token)} claims={keep}"
    except Exception:  # noqa: BLE001 - only a summary
        return f"len={len(token)} (claims not decodable)"


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9883
    host = os.environ["C8Y_DOMAIN"]
    cert_file = os.environ.get("PROBE_CERT_FILE")
    device_id = (os.environ["PROBE_DEVICE_ID"] if cert_file
                 else os.environ["SPIKE_A_DEVICE_ID"])

    received: list[tuple[str, str]] = []
    connected = threading.Event()
    got_jwt = threading.Event()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=device_id,
                         protocol=mqtt.MQTTv311)
    if cert_file:
        client.tls_set(certfile=cert_file, keyfile=os.environ["PROBE_KEY_FILE"],
                       cert_reqs=ssl.CERT_REQUIRED)
    else:
        client.username_pw_set(os.environ["SPIKE_A_MQTT_USER"],
                               os.environ["SPIKE_A_MQTT_PASSWORD"])
        client.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    print(f"auth: {'certificate' if cert_file else 'basic'} as {device_id}")

    def on_connect(c, _u, _f, reason, _p):
        print(f"connect: {reason}")
        if not reason.is_failure:
            # One filter per SUBSCRIBE: the MQTT Service (9883) refuses a
            # SUBSCRIBE that carries several filters.
            for topic in ("s/e", "s/ds", "s/dat"):
                c.subscribe(topic, 1)
            connected.set()

    def on_message(_c, _u, msg):
        text = msg.payload.decode(errors="replace")
        if msg.topic == "s/dat":
            code, _, token = text.partition(",")
            print(f"s/dat: {code},<jwt {jwt_summary(token)}>")
            got_jwt.set()
        else:
            print(f"{msg.topic}: {text}")
        received.append((msg.topic, text))

    client.on_connect = on_connect
    client.on_message = on_message

    t0 = time.monotonic()
    client.connect(host, port, keepalive=60)
    client.loop_start()
    if not connected.wait(15):
        print("FAIL: no CONNACK within 15 s")
        return 1
    print(f"connected to {host}:{port} in {time.monotonic() - t0:.2f} s")
    time.sleep(1)  # let the subscriptions settle

    for line in (f"100,{device_id},thin-edge.io-zephyr-spike",
                 "114,c8y_Restart",
                 "117,60"):
        client.publish("s/us", line, qos=1).wait_for_publish(10)
        print(f"s/us <- {line}")

    client.publish("s/uat", "", qos=1).wait_for_publish(10)
    jwt_ok = got_jwt.wait(10)
    print(f"s/uat -> s/dat: {'ok' if jwt_ok else 'NO RESPONSE'}")

    if port == 9883:
        topic = f"te/device/{device_id}///m/environment"
        body = json.dumps({"temperature": 21.5, "humidity": 40.1})
        info = client.publish(topic, body, qos=1)
        info.wait_for_publish(10)
        print(f"free-form <- {topic} {body} (rc={info.rc})")

    time.sleep(3)  # collect any s/e errors
    client.loop_stop()
    client.disconnect()

    errors = [t for t, _ in received if t == "s/e"]
    print(f"summary: jwt={'ok' if jwt_ok else 'fail'} s/e errors={len(errors)}")
    return 0 if jwt_ok and not errors else 1


if __name__ == "__main__":
    sys.exit(main())
