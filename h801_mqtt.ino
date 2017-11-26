//
// Alternative firmware for H801 5 channel LED dimmer
// based on https://github.com/mertenats/open-home-automation/blob/master/ha_mqtt_rgb_light/ha_mqtt_rgb_light.ino
//
#include <string>

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>   // Local WebServer used to serve the configuration portal
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>        // WiFi Configuration Magic
#include <PubSubClient.h>       // MQTT client
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

WiFiManager wifiManager;
WiFiClient wifiClient;
PubSubClient client(wifiClient);
ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

const char* mqtt_server = "192.168.1.10";
const char* mqtt_user = "";
const char* mqtt_pass = "";
// Password for update server
const char* username = "admin";
const char* password = "homeassistant";

// MQTT topics
// state, brightness, rgb
const char* MQTT_UP = "active";

/* global on/off */
char* MQTT_LIGHT_STATE_TOPIC = "XXXXXXXX/light/status";
char* MQTT_LIGHT_COMMAND_TOPIC = "XXXXXXXX/light/switch";

char* MQTT_LIGHT_RGB_RGB_STATE_TOPIC = "XXXXXXXX/rgb/status";
char* MQTT_LIGHT_RGB_RGB_COMMAND_TOPIC = "XXXXXXXX/rgb/set";
char* MQTT_LIGHT_W1_BRIGHTNESS_STATE_TOPIC = "XXXXXXXX/w1/status";
char* MQTT_LIGHT_W1_BRIGHTNESS_COMMAND_TOPIC = "XXXXXXXX/w1/set";
char* MQTT_LIGHT_W2_BRIGHTNESS_STATE_TOPIC = "XXXXXXXX/w2/status";
char* MQTT_LIGHT_W2_BRIGHTNESS_COMMAND_TOPIC = "XXXXXXXX/w2/set";

char* chip_id = "00000000";
char* myhostname = "esp00000000";

// buffer used to send/receive data with MQTT
const uint8_t MSG_BUFFER_SIZE = 20;
char m_msg_buffer[MSG_BUFFER_SIZE];


// Light
// the payload that represents enabled/disabled state, by default
const char* LIGHT_ON = "ON";
const char* LIGHT_OFF = "OFF";


#define RGB_LIGHT_RED_PIN    15
#define RGB_LIGHT_GREEN_PIN  13
#define RGB_LIGHT_BLUE_PIN   12
#define W1_PIN               14
#define W2_PIN               4

#define GREEN_PIN    1
#define RED_PIN      5

// store the state of the rgb light (colors, brightness, ...)
boolean m_global_on = true;

uint8_t m_rgb_red = 255;
uint8_t m_rgb_green = 255;
uint8_t m_rgb_blue = 255;
uint8_t m_w1_brightness = 255;
uint8_t m_w2_brightness = 255;

void setup()
{
  pinMode(RGB_LIGHT_RED_PIN, OUTPUT);
  pinMode(RGB_LIGHT_GREEN_PIN, OUTPUT);
  pinMode(RGB_LIGHT_BLUE_PIN, OUTPUT);
  analogWriteRange(255);
  setColor(0, 0, 0);
  pinMode(W1_PIN, OUTPUT);
  setW1(0);
  pinMode(W2_PIN, OUTPUT);
  setW2(0);

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

  wifiManager.setTimeout(3600);
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  wifiManager.addParameter(&custom_mqtt_server);
  WiFiManagerParameter custom_password("password", "password for updates", password, 40);
  wifiManager.addParameter(&custom_password);
  WiFiManagerParameter custom_mqtt_user("mqttuser", "mqtt user", mqtt_user, 40);
  wifiManager.addParameter(&custom_mqtt_user);
  WiFiManagerParameter custom_mqtt_pass("mqttpass", "mqtt pass", mqtt_pass, 40);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.setCustomHeadElement(chip_id);
  wifiManager.autoConnect();

  mqtt_server = custom_mqtt_server.getValue();
  mqtt_user = custom_mqtt_user.getValue();
  mqtt_pass = custom_mqtt_pass.getValue();
  password = custom_password.getValue();

  Serial1.println("WiFi connected");
  Serial1.println("IP address: ");
  Serial1.println(WiFi.localIP());
  Serial1.print("Chip ID: ");
  Serial1.println(chip_id);


  // init the MQTT connection
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  // replace chip ID in channel names
  memcpy(MQTT_LIGHT_STATE_TOPIC, chip_id, 8);
  memcpy(MQTT_LIGHT_COMMAND_TOPIC, chip_id, 8);
  memcpy(MQTT_LIGHT_RGB_RGB_STATE_TOPIC, chip_id, 8);
  memcpy(MQTT_LIGHT_RGB_RGB_COMMAND_TOPIC, chip_id, 8);

  memcpy(MQTT_LIGHT_W1_BRIGHTNESS_STATE_TOPIC, chip_id, 8);
  memcpy(MQTT_LIGHT_W1_BRIGHTNESS_COMMAND_TOPIC, chip_id, 8);

  memcpy(MQTT_LIGHT_W2_BRIGHTNESS_STATE_TOPIC, chip_id, 8);
  memcpy(MQTT_LIGHT_W2_BRIGHTNESS_COMMAND_TOPIC, chip_id, 8);

  digitalWrite(RED_PIN, 1);

  // OTA
  // do not start OTA server if no password has been set
  if (password != "") {
    MDNS.begin(myhostname);
    httpUpdater.setup(&httpServer, username, password);
    httpServer.begin();
    MDNS.addService("http", "tcp", 80);
  }
}

// function called to adapt the brightness and the colors of the led
void setColor(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(RGB_LIGHT_RED_PIN, r);
  analogWrite(RGB_LIGHT_GREEN_PIN, g);
  analogWrite(RGB_LIGHT_BLUE_PIN, b);
}

void setW1(uint8_t brightness) {
  analogWrite(W1_PIN, brightness);
}

void setW2(uint8_t brightness) {
  analogWrite(W2_PIN, brightness);
}

void updateLEDs() {
  if (m_global_on) {
    setColor(m_rgb_red, m_rgb_green, m_rgb_blue);
    setW1(m_w1_brightness);
    setW2(m_w2_brightness);
  } else {
    setColor(0, 0, 0);
    setW1(0);
    setW2(0);
  }
}

// function called to publish the state of the led (on/off)
void publishGlobalState() {
  if (m_global_on) {
    client.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_ON, true);
  } else {
    client.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_OFF, true);
  }
}

// function called to publish the colors of the led (xx(x),xx(x),xx(x))
void publishRGBColor() {
  snprintf(m_msg_buffer, MSG_BUFFER_SIZE, "%d,%d,%d", m_rgb_red, m_rgb_green, m_rgb_blue);
  client.publish(MQTT_LIGHT_RGB_RGB_STATE_TOPIC, m_msg_buffer, true);
}

void publishW1Brightness() {
  snprintf(m_msg_buffer, MSG_BUFFER_SIZE, "%d", m_w1_brightness);
  client.publish(MQTT_LIGHT_W1_BRIGHTNESS_STATE_TOPIC, m_msg_buffer, true);
}

void publishW2Brightness() {
  snprintf(m_msg_buffer, MSG_BUFFER_SIZE, "%d", m_w2_brightness);
  client.publish(MQTT_LIGHT_W2_BRIGHTNESS_STATE_TOPIC, m_msg_buffer, true);
}

// function called when a MQTT message arrived
void callback(char* p_topic, byte* p_payload, unsigned int p_length) {
  // concat the payload into a string
  String payload;
  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }

  // handle message topic
  if (String(MQTT_LIGHT_COMMAND_TOPIC).equals(p_topic)) {
    // test if the payload is equal to "ON" or "OFF"
    if (payload.equals(String(LIGHT_ON))) {
      m_global_on = true;
      updateLEDs();
      publishGlobalState();
    } else if (payload.equals(String(LIGHT_OFF))) {
      m_global_on = false;
      updateLEDs();
      publishGlobalState();
    }
  } else if (String(MQTT_LIGHT_W1_BRIGHTNESS_COMMAND_TOPIC).equals(p_topic)) {
    uint8_t brightness = payload.toInt();
    if (brightness < 0 || brightness > 255) {
      // do nothing...
      return;
    } else {
      m_w1_brightness = brightness;
      updateLEDs();
      publishW1Brightness();
    }
  } else if (String(MQTT_LIGHT_W2_BRIGHTNESS_COMMAND_TOPIC).equals(p_topic)) {
    uint8_t brightness = payload.toInt();
    if (brightness < 0 || brightness > 255) {
      // do nothing...
      return;
    } else {
      m_w2_brightness = brightness;
      updateLEDs();
      publishW2Brightness();
    }
  } else if (String(MQTT_LIGHT_RGB_RGB_COMMAND_TOPIC).equals(p_topic)) {
    if (payload.startsWith("#")) {
      const long tmp = strtol(&payload[1], NULL, 16);
      m_rgb_red = tmp >> 16;
      m_rgb_green = tmp >> 8 & 0xff;
      m_rgb_blue = tmp & 0xff;
    } else {
      // get the position of the first and second commas
      uint8_t firstIndex = payload.indexOf(',');
      uint8_t lastIndex = payload.lastIndexOf(',');

      uint8_t rgb_red = payload.substring(0, firstIndex).toInt();
      if (rgb_red < 0 || rgb_red > 255) {
        return;
      } else {
        m_rgb_red = rgb_red;
      }

      uint8_t rgb_green = payload.substring(firstIndex + 1, lastIndex).toInt();
      if (rgb_green < 0 || rgb_green > 255) {
        return;
      } else {
        m_rgb_green = rgb_green;
      }

      uint8_t rgb_blue = payload.substring(lastIndex + 1).toInt();
      if (rgb_blue < 0 || rgb_blue > 255) {
        return;
      } else {
        m_rgb_blue = rgb_blue;
      }
    }

    updateLEDs();
    publishRGBColor();
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
      // blink 10 times green LED for success connected
      for (int x = 0; x < 10; x++) {
        delay(100);
        digitalWrite(GREEN_PIN, 0);
        delay(100);
        digitalWrite(GREEN_PIN, 1);
      }

      client.publish(MQTT_UP, chip_id);
      // Once connected, publish an announcement...
      // publish the initial values
      publishGlobalState();
      publishRGBColor();
      publishW1Brightness();
      publishW2Brightness();
      // ... and resubscribe
      client.subscribe(MQTT_LIGHT_COMMAND_TOPIC);
      client.subscribe(MQTT_LIGHT_RGB_RGB_COMMAND_TOPIC);
      client.subscribe(MQTT_LIGHT_W1_BRIGHTNESS_COMMAND_TOPIC);
      client.subscribe(MQTT_LIGHT_W2_BRIGHTNESS_COMMAND_TOPIC);
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

uint16_t i = 0;

void loop()
{
  // process OTA updates
  httpServer.handleClient();

  i++;
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Post the full status to MQTT every 65535 cycles. This is roughly once a minute
  // this isn't exact, but it doesn't have to be. Usually, clients will store the value
  // internally. This is only used if a client starts up again and did not receive
  // previous messages
  delay(1);
  if (i == 0) {
    publishGlobalState();
    publishRGBColor();
    publishW1Brightness();
    publishW2Brightness();
  }
}

