/*
 * Volumio Simple Control Buttons
 * Copyright (C) 2021  Elehobica
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <ESPmDNS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>

#include "driver/gpio.h"

// Volumio host
const char *VolumioHost = "volumio";
const int VolumioPort = 3000;

// SocketIO status
typedef enum {
  None = 0,
  WiFiNotConnected,
  Disconnected,
  Connected
} socketIO_status_t;

socketIO_status_t socketIO_status = None;

// GPIO Setting
const gpio_num_t PIN_LED           = ((gpio_num_t)  2);
const gpio_num_t PIN_BUTTON_CENTER = ((gpio_num_t) 17);
const gpio_num_t PIN_BUTTON_DOWN   = ((gpio_num_t) 18);
const gpio_num_t PIN_BUTTON_UP     = ((gpio_num_t) 19);

// Configuration for button recognition
const uint32_t RELEASE_IGNORE_COUNT = 8;
const uint32_t LONG_PUSH_COUNT = 10;
const uint32_t LONG_LONG_PUSH_COUNT = 30;

// Button status
typedef enum {
  ButtonOpen = 0,
  ButtonCenter,
  ButtonUp,
  ButtonDown
} button_status_t;

const uint32_t NUM_BTN_HISTORY = 30;
button_status_t button_prv[NUM_BTN_HISTORY];
uint32_t button_repeat_count = LONG_LONG_PUSH_COUNT; // to ignore first buttton press when power-on

// UI action
xQueueHandle ui_evt_queue = NULL;
typedef enum {
  EVT_NONE = 0,
  EVT_TOGGLE,
  EVT_VOLUME_UP,
  EVT_VOLUME_DOWN,
  EVT_RANDOM_ALBUM
} ui_evt_t;

// Task Handles
TaskHandle_t th[2];

// SocketIO Client
SocketIOclient socketIO;

// LED blink parameters
unsigned long led_count = 0;
const int led_blink_start = 20;
const int led_blink_term = 4;
const int led_interval_normal = 500;

void GPIO_init()
{
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON_CENTER, INPUT_PULLUP);
  pinMode(PIN_BUTTON_DOWN, INPUT_PULLUP);
  pinMode(PIN_BUTTON_UP, INPUT_PULLUP);
}

void LED_immediate_on()
{
  led_count = led_blink_start;
}

button_status_t get_button_status()
{
  button_status_t ret;
  if (gpio_get_level(PIN_BUTTON_CENTER) == 0) {
    ret = ButtonCenter;
  } else if (gpio_get_level(PIN_BUTTON_DOWN) == 0) {
    ret = ButtonDown;
  } else if (gpio_get_level(PIN_BUTTON_UP) == 0) {
    ret = ButtonUp;
  } else {
    ret = ButtonOpen;
  }
  return ret;
}

int count_center_clicks()
{
  int detected_fall = 0;
  int clicks = 0;
  for (int i = 0; i < 4; i++) {
    if (button_prv[i] != ButtonOpen) {
      return 0;
    }
  }
  for (int i = 4; i < NUM_BTN_HISTORY; i++) {
    if (detected_fall == 0 && button_prv[i-1] == ButtonOpen && button_prv[i] == ButtonCenter) {
      detected_fall = 1;
    } else if (detected_fall == 1 && button_prv[i-1] == ButtonCenter && button_prv[i] == ButtonOpen) {
      clicks++;
      detected_fall = 0;
    }
  }
  if (clicks > 0) {
    for (int i = 0; i < NUM_BTN_HISTORY; i++) button_prv[i] = ButtonOpen;
  }
  return clicks;
}

void trigger_ui_event(ui_evt_t ui_evt_id)
{
  //Serial.print("Trigger UI Event: ");
  //Serial.println(ui_evt_id);
  xQueueSend(ui_evt_queue, &ui_evt_id, 10 / portTICK_RATE_MS);
}

void update_button_action()
{
  button_status_t button = get_button_status();
  if (button == ButtonOpen) {
    // Ignore button release after long push
    if (button_repeat_count > LONG_PUSH_COUNT) {
      for (int i = 0; i < NUM_BTN_HISTORY; i++) {
        button_prv[i] = ButtonOpen;
      }
      button = ButtonOpen;
    }
    button_repeat_count = 0;
    if (button_prv[RELEASE_IGNORE_COUNT] == ButtonCenter) { // center release
      int center_clicks = count_center_clicks(); // must be called once per tick because button_prv[] status has changed
      switch (center_clicks) {
        case 1:
          trigger_ui_event(EVT_TOGGLE);
          break;
        case 2:
          //trigger_ui_event(EVT_NONE);
          break;
        case 3:
          trigger_ui_event(EVT_RANDOM_ALBUM);
          break;
        default:
          break;
      }
    }
  } else if (button_prv[0] == ButtonOpen) { // push
    if (button == ButtonUp) {
      trigger_ui_event(EVT_VOLUME_UP);
    } else if (button == ButtonDown) {
      trigger_ui_event(EVT_VOLUME_DOWN);
    }
  } else if (button_repeat_count == LONG_PUSH_COUNT) { // long push
    if (button == ButtonCenter) {
      //trigger_ui_event(EVT_NONE);
      button_repeat_count++; // only once and step to longer push event
    } else if (button == ButtonUp) {
      trigger_ui_event(EVT_VOLUME_UP);
    } else if (button == ButtonDown) {
      trigger_ui_event(EVT_VOLUME_DOWN);
    }
  } else if (button_repeat_count == LONG_LONG_PUSH_COUNT) { // long long push
    if (button == ButtonCenter) {
      //trigger_ui_event(EVT_NONE);
    }
    button_repeat_count++; // only once and step to longer push event
  } else if (button == button_prv[0]) {
    button_repeat_count++;
  }
  // Button status shift
  for (int i = NUM_BTN_HISTORY-2; i >= 0; i--) {
    button_prv[i+1] = button_prv[i];
  }
  button_prv[0] = button;
}

void UI_Task(void *pvParameters)
{
  // Initialize
  for (int i = 0; i < NUM_BTN_HISTORY; i++) button_prv[i] = ButtonOpen;
  vTaskDelay(100 / portTICK_PERIOD_MS);
  while (true) {
    update_button_action();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void socketIOEvent(socketIOmessageType_t type, uint8_t * payload, size_t length)
{
  switch(type) {
    case sIOtype_DISCONNECT:
      Serial.printf("[IOc] Disconnected!\n");
      break;
    case sIOtype_CONNECT:
      Serial.printf("[IOc] Connected to url: %s\n", payload);
      // join default namespace (no auto join in Socket.IO V3)
      socketIO.send(sIOtype_CONNECT, "/");
      break;
    case sIOtype_EVENT:
      //Serial.printf("[IOc] get event: %s\n", payload);
      break;
    case sIOtype_ACK:
      Serial.printf("[IOc] get ack: %u\n", length);
      break;
    case sIOtype_ERROR:
      Serial.printf("[IOc] get error: %u\n", length);
      break;
    case sIOtype_BINARY_EVENT:
      Serial.printf("[IOc] get binary: %u\n", length);
      break;
    case sIOtype_BINARY_ACK:
      Serial.printf("[IOc] get binary ack: %u\n", length);
      break;
  } 
}

void emit(String string)
{
  // Send event
  socketIO.sendEVENT(string);
  Serial.print("SocketIO emit: ");
  Serial.println(string);
}

void emitText(const char *event, const char *text)
{
  DynamicJsonDocument doc(256);
  JsonArray array = doc.to<JsonArray>();
  array.add(event);
  array.add(text);

  String output;
  serializeJson(doc, output);

  emit(output);
}

void emitJSON(const char *event, const char *json)
{
  DynamicJsonDocument doc(64 + 256);
  JsonArray array = doc.to<JsonArray>();
  array.add(event);
  DynamicJsonDocument jsonDoc(256);
  deserializeJson(jsonDoc, json);
  array.add(jsonDoc);

  String output;
  serializeJson(doc, output);

  emit(output);
}

bool connectVolumioSocketIO()
{
  IPAddress volumioIpAddr = MDNS.queryHost(VolumioHost);
  if (volumioIpAddr == IPAddress(0, 0, 0, 0)) {
    Serial.println("Can't find Volumio");
    return false;
  }
  Serial.print("Volumio IP: ");
  Serial.println(volumioIpAddr);

  int count = 0;
  // connect to Volumio Socket IO Server (Port: VolumioPort)
  socketIO.begin(volumioIpAddr.toString(), VolumioPort);
  socketIO.onEvent(socketIOEvent);
  while (!socketIO.isConnected()) {
    socketIO.loop();
    delay(1);
    if (count++ > 100) { return false; }
  }
  return true;
}

void SocketIO_Task(void *pvParameters)
{
  socketIO_status = WiFiNotConnected;
  // Launch WiFiManager
  WiFiManager wifiManager;

  if (gpio_get_level(PIN_BUTTON_CENTER) == 0 && gpio_get_level(PIN_BUTTON_DOWN) == 0 && gpio_get_level(PIN_BUTTON_UP) == 0) {
    wifiManager.resetSettings();
  }
  wifiManager.autoConnect("OnDemandAP");
  
  //if you get here you have connected to the WiFi
  socketIO_status = Disconnected;
  IPAddress ipAddr = WiFi.localIP();
  Serial.println("WiFi Connected");
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("Local IP: ");
  Serial.println(ipAddr);

  // search Volumio by mDNS
  mdns_init();
  connectVolumioSocketIO();

  while (true) {
    socketIO.loop();
    socketIO_status = socketIO.isConnected() ? Connected : Disconnected;
    ui_evt_t ui_evt_id;
    if (socketIO_status == Disconnected) { // retry connect every 5 sec
      while (xQueueReceive(ui_evt_queue, &ui_evt_id, 0)); // Ignore UI Event
      connectVolumioSocketIO();
      delay(5000);
      continue;
    }
    // Receive UI Event
    if (xQueueReceive(ui_evt_queue, &ui_evt_id, 0)) {
      switch (ui_evt_id) {
        case EVT_TOGGLE:
          emitJSON("toggle", "{}");
          break;
        case EVT_VOLUME_DOWN:
          emitText("volume", "-");
          break;
        case EVT_VOLUME_UP:
          emitText("volume", "+");
          break;
        case EVT_RANDOM_ALBUM:
          emitJSON("callMethod", "{\"endpoint\": \"miscellanea/randomizer\", \"method\": \"randomAlbum\"}");
          // Wait 3 sec for new play list to be reflected
          for (int i = 0; i < 30; i++) {
            socketIO.loop();
            delay(100);
          }
          emitJSON("play", "{\"value\": 0}");
          break;
        default:
          Serial.printf("illegal UI event %d\n", ui_evt_id);
          break;
      }
      LED_immediate_on();
    }
    delay(10);
  }
}

void setup()
{
  // Serial
  Serial.begin(115200);
  Serial.println("\n Starting");

  // GPIO Initialize
  GPIO_init();

  // Queue Initialize
  ui_evt_queue = xQueueCreate(32, sizeof(ui_evt_t));

  // Start SocketIO task
  xTaskCreatePinnedToCore(SocketIO_Task, "SocketIO_Task", 1024*4, NULL, 5, &th[0], 0);

  // Start UI task (button detection)
  xTaskCreatePinnedToCore(UI_Task, "UI_Task", 1024*4, NULL, 5, &th[1], 0);

  gpio_set_level(PIN_LED, 1);
}

void loop()
{
  // LED blink control
  if (led_count > led_blink_start - led_blink_term && led_count <= led_blink_start) {
    gpio_set_level(PIN_LED, 1);
  } else {
    gpio_set_level(PIN_LED, 0);
  }
  if (led_count-- <= 0) {
    led_count = (socketIO_status == Disconnected) ? led_blink_start : led_interval_normal;
  }
  delay(10);
}
