import json
import time
import logging
import requests
from datetime import datetime, timezone
from dotenv import load_dotenv
import os
import paho.mqtt.client as mqtt

# Load environment variables
load_dotenv()

MQTT_BROKER   = os.getenv("MQTT_BROKER", "broker.hivemq.com")
MQTT_PORT     = int(os.getenv("MQTT_PORT", 1883))
MQTT_TOPIC    = os.getenv("MQTT_TOPIC", "Group6_SmartFarm")
MQTT_CLIENT   = "smartfarm-bridge"

MQTT_USERNAME = os.getenv("MQTT_USERNAME")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")
MQTT_CA_CERT  = os.getenv("MQTT_CA_CERT")

FIREBASE_URL  = os.getenv("FIREBASE_URL")   
FIREBASE_AUTH = os.getenv("FIREBASE_AUTH")  

# Logging 
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S"
)
log = logging.getLogger("smartfarm-bridge")

# Firebase helpers 
def firebase_post(endpoint: str, payload: dict) -> bool:
    """POST data to a Firebase endpoint. Returns True on success."""
    url = f"{FIREBASE_URL}/{endpoint}.json?auth={FIREBASE_AUTH}"
    try:
        r = requests.post(url, json=payload, timeout=10)
        r.raise_for_status()
        return True
    except requests.RequestException as e:
        log.error(f"Firebase POST failed ({endpoint}): {e}")
        return False


def firebase_put(endpoint: str, payload: dict) -> bool:
    """PUT (overwrite) data at a Firebase endpoint. Returns True on success."""
    url = f"{FIREBASE_URL}/{endpoint}.json?auth={FIREBASE_AUTH}"
    try:
        r = requests.put(url, json=payload, timeout=10)
        r.raise_for_status()
        return True
    except requests.RequestException as e:
        log.error(f"Firebase PUT failed ({endpoint}): {e}")
        return False


def write_to_firebase(data: dict) -> None:
    """
    Write a sensor reading to Firebase.
    - POST to /readings  → appends with auto-generated key (historical archive)
    - PUT  to /latest    → overwrites with current values (fast live read)
    - PUT  to /device    → updates heartbeat
    """
    now = datetime.now(timezone.utc).isoformat()
    data["timestamp"] = now

    # 1. Append to historical readings
    ok_history = firebase_post("readings", data)
    if ok_history:
        log.info("Firebase /readings — new record written.")

    # 2. Overwrite latest node
    ok_latest = firebase_put("latest", data)
    if ok_latest:
        log.info("Firebase /latest — updated.")

    # 3. Update device heartbeat
    device_payload = {
        "status":      "online",
        "last_seen":   now,
        "mqtt_active": True,
        "ip_address":  "via-mqtt"
    }
    firebase_put("device", device_payload)


# MQTT callbacks 
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        log.info(f"Connected to MQTT broker: {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe(MQTT_TOPIC)
        log.info(f"Subscribed to topic: {MQTT_TOPIC}")
    else:
        log.error(f"MQTT connection failed — return code {rc}")


def on_disconnect(client, userdata, rc):
    log.warning(f"MQTT disconnected (rc={rc}). Will auto-reconnect.")
    # Mark device offline in Firebase when bridge loses MQTT connection
    firebase_put("device", {
        "status":      "offline",
        "last_seen":   datetime.now(timezone.utc).isoformat(),
        "mqtt_active": False,
        "ip_address":  "unknown"
    })


def on_message(client, userdata, msg):
    """Called each time a message arrives on the subscribed topic."""
    try:
        raw = msg.payload.decode("utf-8")
        log.info(f"MQTT message received on [{msg.topic}]: {raw}")
        data = json.loads(raw)
        validate_and_write(data)
    except json.JSONDecodeError as e:
        log.error(f"Invalid JSON payload: {e}")
    except Exception as e:
        log.error(f"Unexpected error processing message: {e}")


# Validation 
REQUIRED_FIELDS = {"soil_moisture", "temperature", "humidity", "light_level"}

def validate_and_write(data: dict) -> None:
    """Validate payload fields before writing to Firebase."""
    missing = REQUIRED_FIELDS - data.keys()
    if missing:
        log.warning(f"Payload missing fields: {missing} — skipping write.")
        return

    # Sanity check ranges
    if not (0 <= data["soil_moisture"] <= 100):
        log.warning(f"Soil moisture out of range: {data['soil_moisture']}")
    if not (-10 <= data["temperature"] <= 60):
        log.warning(f"Temperature out of range: {data['temperature']}")
    if not (0 <= data["humidity"] <= 100):
        log.warning(f"Humidity out of range: {data['humidity']}")
    if not (0 <= data["light_level"] <= 100):
        log.warning(f"Light level out of range: {data['light_level']}")

    # Recalculate alerts server-side as a safety net
    # (ESP32 also sends alerts but bridge recalculates to be sure)
    data["alerts"] = {
        "irrigate":    data["soil_moisture"] < 30.0,
        "heat_stress": data["temperature"]   > 35.0,
        "dry_air":     data["humidity"]      < 40.0,
        "poor_light":  data["light_level"]     < 30.0
    }

    write_to_firebase(data)


# Main 
def main():
    if not FIREBASE_URL or not FIREBASE_AUTH:
        log.critical("FIREBASE_URL and FIREBASE_AUTH must be set in .env — aborting.")
        return

    client = mqtt.Client(client_id=MQTT_CLIENT, clean_session=True)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message

    # Reconnect settings — keeps bridge alive if broker drops
    client.reconnect_delay_set(min_delay=2, max_delay=30)

    log.info(f"Connecting to MQTT broker: {MQTT_BROKER}:{MQTT_PORT}")
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)

    try:
        client.loop_forever()  # blocking — handles reconnects automatically
    except KeyboardInterrupt:
        log.info("Bridge stopped by user.")
        client.disconnect()


if __name__ == "__main__":
    main()
