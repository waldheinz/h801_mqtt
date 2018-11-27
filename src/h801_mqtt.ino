//
// Alternative firmware for H801 5 channel LED dimmer
// based on
// https://github.com/mertenats/open-home-automation/blob/master/ha_mqtt_rgb_light/ha_mqtt_rgb_light.ino
//
#include <string>
#include <sstream>

#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h> // Local WebServer used to serve the configuration portal
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <PubSubClient.h> // MQTT client
#include <WiFiManager.h>  // WiFi Configuration Magic
#include <WiFiUdp.h>

extern "C" {
    #include "ESP8266_new_pwm.h"
}

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

uint8_t const m_global_fade_nom = 10;

uint8_t const PWM_CHANNELS = 5; // RGBWW
uint32_t const PWM_PERIOD = 4096; // * 200ns ^= 1 kHz

uint32 io_info[PWM_CHANNELS][3] = {
	// MUX, FUNC, PIN
	{ PERIPHS_IO_MUX_MTDI_U,  FUNC_GPIO15, RGB_LIGHT_RED_PIN },
	{ PERIPHS_IO_MUX_MTDO_U,  FUNC_GPIO13, RGB_LIGHT_GREEN_PIN },
	{ PERIPHS_IO_MUX_MTCK_U,  FUNC_GPIO12, RGB_LIGHT_BLUE_PIN },
	{ PERIPHS_IO_MUX_MTMS_U,  FUNC_GPIO14, W1_PIN },
    { PERIPHS_IO_MUX_GPIO4_U,  FUNC_GPIO4, W2_PIN },
};

struct led_state {
    uint32_t r, g, b, w1, w2;

    led_state() { r = g = b = w1 = w2 = 0; }

    void set_r(uint8_t v)  { this->r  = v << 4; }
    void set_g(uint8_t v)  { this->g  = v << 4; }
    void set_b(uint8_t v)  { this->b  = v << 4; }
    void set_w1(uint8_t v) { this->w1 = v << 4; }
    void set_w2(uint8_t v) { this->w2 = v << 4; }

    bool approach(led_state const & tgt) {
        bool result = false;

        for (int i=0; i < 5; i++) {
            auto const c = (*this)[i];
            auto const t = tgt[i];

            if (c != t) {
                result = true;

                if (abs(c - t) <= m_global_fade_nom) {
                    (*this)[i] = t;
                } else {
                    (*this)[i] += t > c ? m_global_fade_nom : -m_global_fade_nom;
                }
            }
        }

        return result;
    }

    uint32_t const & operator[](uint8_t idx) const {
        switch (idx) {
            case 0: return r;
            case 1: return g;
            case 2: return b;
            case 3: return w1;
            default: return w2;
        }
    }

    uint32_t & operator[](uint8_t idx) {
        switch (idx) {
            case 0: return r;
            case 1: return g;
            case 2: return b;
            case 3: return w1;
            default: return w2;
        }
    }

    void apply() const {
        pwm_set_duty(r, 0);  // GPIO15: 10%
        pwm_set_duty(g, 1);  // GPIO15: 100%
        pwm_set_duty(b, 2);  // GPIO15: 100%
        pwm_set_duty(w1, 3); // GPIO15: 100%
        pwm_set_duty(w2, 4); // GPIO15: 100%
        pwm_start();         // commit
    }
};

led_state led_current;
led_state led_target;
led_state const leds_off;

void setup() {
    analogWriteRange(PWMRANGE);
    pinMode(RGB_LIGHT_RED_PIN, OUTPUT);
    pinMode(RGB_LIGHT_GREEN_PIN, OUTPUT);
    pinMode(RGB_LIGHT_BLUE_PIN, OUTPUT);
    pinMode(W1_PIN, OUTPUT);
    pinMode(W2_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);
    pinMode(RED_PIN, OUTPUT);

    // initial duty: all off
    uint32 pwm_duty_init[PWM_CHANNELS] = {0, 0, 0, 0, 0};
    pwm_init(PWM_PERIOD, pwm_duty_init, PWM_CHANNELS, io_info);
    pwm_start();

    // leds_off.apply();

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

    if (ends_with(p_topic, "set/all")) {

        for (int i=0, start = 0; i < 5; i++) {
            int const end = payload.indexOf(',', start);

            led_target[i] = static_cast<uint32_t>(payload.substring(start, end).toInt());

            if (end < 0) {
                break;
            }

            start = end + 1;
        }

    } else if (ends_with(p_topic, "rgb/set")) {
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
            led_target.set_w1(brightness);
        }
    } else if (ends_with(p_topic, "w2/set")) {
        uint8_t brightness = payload.toInt();
        if (brightness < 0 || brightness > 255) {
            return;
        } else {
            led_target.set_w2(brightness);
        }
    }
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
    if (led_current.approach(led_target)) {
        led_current.apply();
    }

    // process OTA updates
    httpServer.handleClient();
    ArduinoOTA.handle();

    if (!client.connected()) {
        leds_off.apply();
        reconnect();
    }

    client.loop();

    delay(1);
}
