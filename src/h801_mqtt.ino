//
// Alternative firmware for H801 5 channel LED dimmer
// based on
// https://github.com/mertenats/open-home-automation/blob/master/ha_mqtt_rgb_light/ha_mqtt_rgb_light.ino
//
#include <string>

#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h> // Local WebServer used to serve the configuration portal
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h> // MQTT client
#include <WiFiManager.h>  // WiFi Configuration Magic
#include <WiFiUdp.h>

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient client(wifiClient);
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

const char *mqtt_server = "blackbox";
const char *mqtt_user = "";
const char *mqtt_pass = "";

char *chip_id = "00000000";
char *myhostname = "esp00000000";

// Light
// the payload that represents enabled/disabled state, by default
const char *LIGHT_ON = "ON";
const char *LIGHT_OFF = "OFF";

#define RGB_LIGHT_RED_PIN 15
#define RGB_LIGHT_GREEN_PIN 13
#define RGB_LIGHT_BLUE_PIN 12
#define W1_PIN 14
#define W2_PIN 4

#define GREEN_PIN 1
#define RED_PIN 5

// store the state of the rgb light (colors, brightness, ...)
boolean m_global_on = true;

struct led_state {
    uint16_t r, g, b, w1, w2;

    led_state() { r = g = b = w1 = w2 = 0; }

    void set_r(uint8_t _r) { this->r = _r * 4; }

    void set_g(uint8_t _g) { this->g = _g * 4; }

    void set_b(uint8_t _b) { this->b = _b * 4; }

    void approach(const led_state &tgt) {
        this->r += (tgt.r > this->r) ? 1 : (tgt.r < this->r) ? -1 : 0;
        this->g += (tgt.g > this->g) ? 1 : (tgt.g < this->g) ? -1 : 0;
        this->b += (tgt.b > this->b) ? 1 : (tgt.b < this->b) ? -1 : 0;
        this->w1 += (tgt.w1 > this->w1) ? 1 : (tgt.w1 < this->w1) ? -1 : 0;
        this->w2 += (tgt.w2 > this->w2) ? 1 : (tgt.w2 < this->w2) ? -1 : 0;
    }
};

led_state led_current;
led_state led_target;
const led_state leds_off;

void setup() {
    led_target.r = PWMRANGE;
    led_target.g = PWMRANGE;
    led_target.b = PWMRANGE;
    led_target.w1 = PWMRANGE;
    led_target.w2 = PWMRANGE;

    analogWriteRange(PWMRANGE);
    pinMode(RGB_LIGHT_RED_PIN, OUTPUT);
    pinMode(RGB_LIGHT_GREEN_PIN, OUTPUT);
    pinMode(RGB_LIGHT_BLUE_PIN, OUTPUT);
    pinMode(W1_PIN, OUTPUT);
    pinMode(W2_PIN, OUTPUT);

    apply_state(leds_off);

    pinMode(GREEN_PIN, OUTPUT);
    pinMode(RED_PIN, OUTPUT);
    digitalWrite(RED_PIN, 0);
    digitalWrite(GREEN_PIN, 1);

    sprintf(chip_id, "%08X", ESP.getChipId());
    sprintf(myhostname, "esp%08X", ESP.getChipId());

    // Setup console
    Serial1.begin(115200);
    delay(10);
    Serial1.println();
    Serial1.println();

    // reset if necessary
    // wifiManager.resetSettings();

    wifiManager.setTimeout(600);
    WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 40);
    wifiManager.addParameter(&custom_mqtt_server);
    WiFiManagerParameter custom_mqtt_user("mqttuser", "MQTT User", mqtt_user, 40);
    wifiManager.addParameter(&custom_mqtt_user);
    WiFiManagerParameter custom_mqtt_pass("mqttpass", "MQTT Password", mqtt_pass, 40);
    wifiManager.addParameter(&custom_mqtt_pass);
    wifiManager.setCustomHeadElement(chip_id);
    wifiManager.autoConnect();

    mqtt_server = custom_mqtt_server.getValue();
    mqtt_user = custom_mqtt_user.getValue();
    mqtt_pass = custom_mqtt_pass.getValue();

    Serial1.println("WiFi connected");
    Serial1.println("IP address: ");
    Serial1.println(WiFi.localIP());
    Serial1.print("Chip ID: ");
    Serial1.println(chip_id);

    // init the MQTT connection
    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);

    digitalWrite(RED_PIN, 1);
    ArduinoOTA.begin();

    // OTA
    MDNS.begin(myhostname);
    httpUpdater.setup(&httpServer);

    httpServer.begin();
    MDNS.addService("http", "tcp", 80);
}

void apply_state(const led_state &s) {
    analogWrite(RGB_LIGHT_RED_PIN, s.r);
    analogWrite(RGB_LIGHT_GREEN_PIN, s.g);
    analogWrite(RGB_LIGHT_BLUE_PIN, s.b);
    analogWrite(W1_PIN, s.w1);
    analogWrite(W2_PIN, s.w2);
}

bool ends_with(std::string const & value, std::string const & ending) {
    if (ending.size() > value.size()) {
        return false;
    } else {
        return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
    }
}

// function called when a MQTT message arrived
void callback(char const * p_topic, byte * p_payload, unsigned int p_length) {
    // concat the payload into a string
    String payload;
    for (uint8_t i = 0; i < p_length; i++) {
        payload.concat((char)p_payload[i]);
    }

    std::string const pl(reinterpret_cast<char const *>(p_payload), p_length);

    if (ends_with(p_topic, "rgb/set")) {
        if (payload.startsWith("#")) {
            const long tmp = strtol(&payload[1], NULL, 16);
            led_target.set_r(tmp >> 16);
            led_target.set_g(tmp >> 8 & 0xff);
            led_target.set_b(tmp & 0xff);
        } else {
            // get the position of the first and second commas
            uint8_t firstIndex = payload.indexOf(',');
            uint8_t lastIndex = payload.lastIndexOf(',');

            uint8_t rgb_red = payload.substring(0, firstIndex).toInt();
            if (rgb_red < 0 || rgb_red > 255) {
                return;
            } else {
                led_target.set_r(rgb_red);
            }

            uint8_t rgb_green = payload.substring(firstIndex + 1, lastIndex).toInt();
            if (rgb_green < 0 || rgb_green > 255) {
                return;
            } else {
                led_target.set_g(rgb_green);
            }

            uint8_t rgb_blue = payload.substring(lastIndex + 1).toInt();
            if (rgb_blue < 0 || rgb_blue > 255) {
                return;
            } else {
                led_target.set_b(rgb_blue);
            }
        }
    } else if (ends_with(p_topic, "w1/set")) {
        uint8_t brightness = payload.toInt();
        if (brightness < 0 || brightness > 255) {
            // do nothing...
            return;
        } else {
            led_target.w1 = brightness << 2;
        }
    } else if (ends_with(p_topic, "w2/set")) {
        uint8_t brightness = payload.toInt();
        if (brightness < 0 || brightness > 255) {
            return;
        } else {
            led_target.w2 = brightness << 2;
        }
    } else if (ends_with(p_topic, "switch")) {
        // test if the payload is equal to "ON" or "OFF"
        if (payload.equals(String(LIGHT_ON))) {
            m_global_on = true;
        } else if (payload.equals(String(LIGHT_OFF))) {
            m_global_on = false;
        }
    }

    digitalWrite(GREEN_PIN, 0);
    delay(1);
    digitalWrite(GREEN_PIN, 1);
}

void reconnect() {
    // Loop until we're reconnected
    while (!client.connected()) {
        Serial1.print("Attempting MQTT connection...");
        // Attempt to connect
        if (client.connect(chip_id, mqtt_user, mqtt_pass)) {
            Serial1.println("connected");
            // blink green LED for success connected

            for (int x = 0; x < 5; x++) {
                delay(100);
                digitalWrite(GREEN_PIN, 0);
                delay(100);
                digitalWrite(GREEN_PIN, 1);
            }

            // ... and resubscribe
            std::string const topic = std::string(chip_id) + "/#";
            client.subscribe(topic.c_str());
        } else {
            Serial1.print("failed, rc=");
            Serial1.print(client.state());
            Serial1.print(mqtt_server);
            Serial1.println(" try again in 5 seconds");
            // Wait about 5 seconds (10 x 500ms) before retrying
            for (int x = 0; x < 10; x++) {
                delay(400);
                digitalWrite(RED_PIN, 0);
                delay(100);
                digitalWrite(RED_PIN, 1);
            }
        }
    }
}

void loop() {
    if (m_global_on && client.connected()) {
        led_current.approach(led_target);
    } else {
        led_current.approach(leds_off);
    }

    apply_state(led_current);

    // process OTA updates
    httpServer.handleClient();
    ArduinoOTA.handle();

    if (!client.connected()) {
        reconnect();
    }

    client.loop();

    delay(2);
}
