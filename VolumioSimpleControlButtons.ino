#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>

#include "driver/gpio.h"

// GPIO Setting
#define GPIO_OUTPUT_LED   ((gpio_num_t) 2)
#define GPIO_INPUT_IO_17  ((gpio_num_t) 17)
#define GPIO_INPUT_IO_18  ((gpio_num_t) 18)
#define GPIO_INPUT_IO_19  ((gpio_num_t) 19)

// GPIO Filter Setting
#define NUM_SW_SHORT_FILTER 5
#define NUM_SW_LONG_FILTER  10

// U/I action
xQueueHandle ui_evt_queue = NULL;
typedef enum {
  EVT_TOGGLE = 0,
  EVT_VOLUME_UP,
  EVT_VOLUME_DOWN
} ui_evt_t;

// Task Handles
TaskHandle_t th;

// SocketIO Client
SocketIOclient socketIO;

// 
unsigned long lastMillis = 0;

void emit(const char *event, const char *data = "")
{
  DynamicJsonDocument doc(1024);
  JsonArray array = doc.to<JsonArray>();
  array.add(event);
  if (strlen(data) == 0) {
    JsonObject param1 = array.createNestedObject();
  } else {
    array.add(data);
  }

  String output;
  serializeJson(doc, output);

  // Send event
  socketIO.sendEVENT(output);
  Serial.println(output);
}

void GPIO_init()
{
  pinMode(GPIO_OUTPUT_LED, OUTPUT);
  pinMode(GPIO_INPUT_IO_17, INPUT_PULLUP);
  pinMode(GPIO_INPUT_IO_18, INPUT_PULLUP);
  pinMode(GPIO_INPUT_IO_19, INPUT_PULLUP);
}

void GPIO_Task(void *pvParameters)
{
  uint32_t sw;
  uint32_t sw_short_filter[NUM_SW_SHORT_FILTER];
  uint32_t sw_long_filter[NUM_SW_LONG_FILTER];
  uint32_t sw_short_filter_all[2];
  uint32_t sw_short_filter_rise;
  uint32_t sw_long_filter_keep;
  uint32_t sw_total_status;
  for (int i = 0; i < NUM_SW_SHORT_FILTER; i++) sw_short_filter[i] = 0;
  for (int i = 0; i < NUM_SW_LONG_FILTER; i++) sw_long_filter[i] = 0;
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  for (int count = 0; ; count++) {
    // SHORT FILTER (detect rising edge)
    for (int i = NUM_SW_SHORT_FILTER-1; i >= 1; i--) {
      sw_short_filter[i] = sw_short_filter[i-1];
    }
    sw = 0;
    sw |= (~gpio_get_level(GPIO_INPUT_IO_17)&0x1)<<0;
    sw |= (~gpio_get_level(GPIO_INPUT_IO_18)&0x1)<<1;
    sw |= (~gpio_get_level(GPIO_INPUT_IO_19)&0x1)<<2;
    sw_short_filter[0] = sw;
    if (count % NUM_SW_SHORT_FILTER == 0) {
      sw_short_filter_all[1] = sw_short_filter_all[0];
      sw_short_filter_all[0] = 0xffffffff;
      for (int i = 0; i < NUM_SW_SHORT_FILTER; i++) {
        sw_short_filter_all[0] &= sw_short_filter[i];
      }
      sw_short_filter_rise = ~sw_short_filter_all[1] & sw_short_filter_all[0];
    } else {
      sw_short_filter_rise = 0x00000000;
    }
    // LONG FILTER (detect pushing)
    if (count % 15 == 0) {
      for (int i = NUM_SW_LONG_FILTER-1; i >= 1; i--) {
        sw_long_filter[i] = sw_long_filter[i-1];
      }
      sw_long_filter[0] = sw_short_filter_all[0];
      sw_long_filter_keep = 0xffffffff;
      for (int i = 0; i < NUM_SW_LONG_FILTER; i++) {
        sw_long_filter_keep &= sw_long_filter[i];
      }
    } else {
      sw_long_filter_keep = 0x00000000;
    }
    // TOTAL (short & long)
    sw_total_status = sw_short_filter_rise | (sw_long_filter_keep & 0b110); // Use short & long
    //sw_total_status = sw_short_filter_rise; // Use short only

    // Process
    if (sw_total_status & (1<<0)) {
      ui_evt_t ui_evt_id = EVT_TOGGLE;
      xQueueSend(ui_evt_queue, &ui_evt_id, 10 / portTICK_RATE_MS);
    }
    if (sw_total_status & (1<<1)) {
      ui_evt_t ui_evt_id = EVT_VOLUME_DOWN;
      xQueueSend(ui_evt_queue, &ui_evt_id, 10 / portTICK_RATE_MS);
    }
    if (sw_total_status & (1<<2)) {
      ui_evt_t ui_evt_id = EVT_VOLUME_UP;
      xQueueSend(ui_evt_queue, &ui_evt_id, 10 / portTICK_RATE_MS);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
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

void setup()
{
  Serial.begin(115200);
  Serial.println("\n Starting");

  // Launch WiFiManager
  WiFiManager wifiManager;

  wifiManager.autoConnect("OnDemandAP");
  
  //if you get here you have connected to the WiFi
  IPAddress ipAddr = WiFi.localIP();

  Serial.println("Connected");
  Serial.println("Local IP");
  Serial.println(ipAddr);
  Serial.println(WiFi.SSID());

  // connect to Volumio Socket IO Server
  socketIO.begin("192.168.0.24", 3000);
  socketIO.onEvent(socketIOEvent);

  // GPIO Task
  GPIO_init();
  ui_evt_queue = xQueueCreate(32, sizeof(uint8_t)); // queue length = 32
  xTaskCreatePinnedToCore(GPIO_Task, "GPIO_Task", 8192, NULL, 5, &th, 0);

  gpio_set_level(GPIO_OUTPUT_LED, 1);
}

void loop()
{
  uint64_t now = millis();
  socketIO.loop();

  // Receive UI Event
  ui_evt_t ui_evt_id;
  if (xQueueReceive(ui_evt_queue, &ui_evt_id, 0)) {
    switch (ui_evt_id) {
      case EVT_TOGGLE:
        emit("toggle");
        break;
      case EVT_VOLUME_DOWN:
        emit("volume", "-");
        break;
      case EVT_VOLUME_UP:
        emit("volume", "+");
        break;
      default:
        break;
    }
    lastMillis = now;
  }

  // LED tick (Periodically and at UI Event)
  if (now - lastMillis > 5000) {
    lastMillis = now;
  }
  if (now - lastMillis < 10) {
    gpio_set_level(GPIO_OUTPUT_LED, 1);
  } else {
    gpio_set_level(GPIO_OUTPUT_LED, 0);
  }
}
