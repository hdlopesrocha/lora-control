#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Dictionary.h>
#include <WiFi.h>
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include <ESPAsyncWebServer.h>
#include <ESP32Time.h>
#include <List.hpp>

#define SERVER 1
#define INVERT_RELAY 1

#define SCK 5    // GPIO5  -- SCK
#define MISO 19  // GPIO19 -- MISO
#define MOSI 27  // GPIO27 -- MOSI
#define SS 18    // GPIO18 -- CS
#define RST 23   // GPIO14 -- RESET (If Lora does not work, replace it with GPIO14)
#define DI0 26   // GPIO26 -- IRQ(Interrupt Request)
#define BAND 868E6
#define OLED_RESET 4
#define RELAY1_PIN 25
#define RELAY2_PIN 25
#define MESSAGE_TYPE_PING 1
#define MESSAGE_TYPE_CONTROL 2
#define MESSAGE_TYPE_EVENT 3

typedef enum {
  SECONDLY,
  MINUTELY,
  HOURLY,
  DAILY,
  WEEKLY,
  MONTHLY,
  YEARLY
} CalendarRepeatFreq;

typedef struct {
  char uid[48];       //check RFC 7986
  char nodeId[32];    //check RFC 7986
  char deviceId[32];  //check RFC 7986
  time_t repeat;
  uint8_t count;
  uint8_t interval;
  time_t start;
  time_t end;
  CalendarRepeatFreq freq;
  bool onGoing;
  bool active;
} CalendarEvent;

typedef struct {
  uint8_t index;
  CalendarEvent event;
} EventMessage;

typedef struct {
  char nodeId[32];    //check RFC 7986
  char deviceId[32];  //check RFC 7986
  long captcha;
  bool value;
} DeviceEvent;

typedef struct {
  int packetSize;
  uint8_t *packet;
} Data;

typedef struct {
  char filename[128];
  size_t index;
  uint8_t *data;
  size_t len;
  bool isFinal;
} FilePart;


typedef struct {
  char deviceId[32];
  bool state;
} DeviceInfo;

typedef struct {
  char nodeId[32];  //check RFC 7986
  time_t time;
  long captcha;
  List<DeviceInfo *> *devices;
} NodeInfo;

unsigned int counter = 0;
ESP32Time rtc(3600);  // offset in seconds GMT+1
Adafruit_SSD1306 display(OLED_RESET);
#ifdef SERVER
const char *nodeId = "Server";
#else
const char *nodeId = "Node1A";
#endif

String secret = "fseCxV7BeM";
long captcha = esp_random();
List<NodeInfo *> nodeInfo;
String myIP = "0.0.0.0";


const char *ntpServer = "pool.ntp.org";
bool haveTime = false;
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 3600;
// Replace with your network credentials


#ifdef SERVER
const char *ssid = "extension2.4G";
const char *password = "goodlife";
#else
const char *ssid = nodeId;
const char *password = "goodlife";
#endif

AsyncWebServer server(80);
boolean relay1Status = true;
boolean relay2Status = true;

void setLocalState(String deviceId, bool state) {
  //Serial.printf("\tsetLocalState(%s=%s)\n", deviceId.c_str(), state ? "true" : "false");
  if (!deviceId.compareTo("r1")) {
    if (relay1Status != state) {
      relay1Status = state;
      digitalWrite(RELAY1_PIN, state ^ INVERT_RELAY ? HIGH : LOW);  // turn the LED on (HIGH is the voltage level)
    }
  }
  if (!deviceId.compareTo("r2")) {
    if (relay2Status != state) {
      relay2Status = state;
      digitalWrite(RELAY2_PIN, state ^ INVERT_RELAY ? HIGH : LOW);  // turn the LED on (HIGH is the voltage level)
    }
  }
}

bool startsWith(const char *a, const char *b) {
  if (strncmp(a, b, strlen(b)) == 0) return 1;
  return 0;
}

#define FILE_LINE_SIZE 512
static CalendarEvent *calendarEvent = NULL;
static List<CalendarEvent *> calendarEvents;
SemaphoreHandle_t calendarEventsMutex = xSemaphoreCreateMutex();  // Create the mutex
int line = 0;
static char fileLine[FILE_LINE_SIZE];
static char fileLineIndex = 0;
static bool wasNewLine = false;
static bool ignoreCurrentChar = false;
static char TEMP_CHARS[FILE_LINE_SIZE];


char *advanceToChar(char *line, char c) {
  char *value = strchr(line, ':');

  if (value != NULL) {
    return ++value;
  } else {
    return NULL;
  }
}

time_t getTimeFromString(char *line) {
  // 20240807T150000
  struct tm timeinfo = { 0 };

  sscanf(line, "%04d%02d%02dT%02d%02d%02d", &timeinfo.tm_year, &timeinfo.tm_mon, &timeinfo.tm_mday, &timeinfo.tm_hour, &timeinfo.tm_min, &timeinfo.tm_sec);
  --timeinfo.tm_mon;
  timeinfo.tm_year -= 1900;

  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");


  return mktime(&timeinfo);
}
char *replace_char(char *str, char find, char replace) {
  char *current_pos = strchr(str, find);
  while (current_pos) {
    *current_pos = replace;
    current_pos = strchr(current_pos, find);
  }
  return str;
}

void handleFileUploadTask(void *parameter) {
  FilePart *filePart = (FilePart *)parameter;

  char *filename = filePart->filename;
  size_t index = filePart->index;
  uint8_t *data = filePart->data;
  size_t len = filePart->len;
  bool isFinal = filePart->isFinal;
  if (xSemaphoreTake(calendarEventsMutex, portMAX_DELAY)) {

    if (!index) {
      Serial.printf("UploadStart: %s\n", filename);
      cleanCalendarEvents();
      line = 0;
    }

    Serial.printf("handleFileUpload %d %d %s\n", index, len, isFinal ? "true" : "false");

    for (int i = 0; i < len; ++i) {
      ignoreCurrentChar = false;
      char c = data[i];
      if (c == '\n' || c == '\r') {
        ignoreCurrentChar = true;
      } else if (c == ' ' && wasNewLine) {
        ignoreCurrentChar = true;
      } else if (wasNewLine) {
        fileLineIndex = 0;
      }

      if (c == '\n') {
        fileLine[fileLineIndex] = '\0';
        // Serial.printf("%d\t%s\n", ++line,fileLine);
        if (startsWith(fileLine, "BEGIN:VEVENT")) {
          Serial.printf("Created instance of CalendarEvent(sizeOf=%d)... ", sizeof(CalendarEvent));
          calendarEvent = new CalendarEvent();
          strcpy(calendarEvent->uid, "no-uid");
          strcpy(calendarEvent->nodeId, "no-node");
          strcpy(calendarEvent->deviceId, "no-device");
          calendarEvent->repeat = 0l;
          calendarEvent->start = 0l;
          calendarEvent->end = 0l;
          calendarEvent->count = 0;
          calendarEvent->interval = 1;
          calendarEvent->onGoing = false;
          calendarEvent->active = true;
          Serial.printf("Ok!\n");
        } else if (startsWith(fileLine, "END:VEVENT") && calendarEvent != NULL) {
          Serial.printf("Event(uid=%s,node=%s,device=%s,start=%jd,end=%jd,freq=%d,repeat=%jd,count=%d,interval=%d)\n", calendarEvent->uid, calendarEvent->nodeId, calendarEvent->deviceId, calendarEvent->start, calendarEvent->end, calendarEvent->freq, calendarEvent->repeat, calendarEvent->count, calendarEvent->interval);
          calendarEvents.add(calendarEvent);
          calendarEvent = NULL;
        } else if (startsWith(fileLine, "UID") && calendarEvent != NULL) {
          strcpy(calendarEvent->uid, advanceToChar(fileLine, ':'));
        } else if (startsWith(fileLine, "DTSTART") && calendarEvent != NULL) {
          calendarEvent->start = getTimeFromString(advanceToChar(fileLine, ':'));
        } else if (startsWith(fileLine, "DTEND") && calendarEvent != NULL) {
          calendarEvent->end = getTimeFromString(advanceToChar(fileLine, ':'));
        } else if (startsWith(fileLine, "LOCATION") && calendarEvent != NULL) {
          char *value = advanceToChar(fileLine, ':');
          char *nodeStr = strstr(value, "NODE=");
          char *deviceStr = strstr(value, "DEVICE=");
          Serial.printf("value=%s\n", value);

          replace_char(value, ';', ' ');

          if (nodeStr != NULL) {
            sscanf(nodeStr, "NODE=%s", calendarEvent->nodeId);  //TODO: Fix did has everything, now it can't contain spaces
            Serial.printf("node=%s\n", calendarEvent->nodeId);
          }
          if (deviceStr != NULL) {
            sscanf(deviceStr, "DEVICE=%s", calendarEvent->deviceId);  //TODO: Fix did has everything, now it can't contain spaces
            Serial.printf("device=%s\n", calendarEvent->deviceId);
          }
        } else if (startsWith(fileLine, "RRULE") && calendarEvent != NULL) {
          char *value = advanceToChar(fileLine, ':');
          char *freqStr = strstr(value, "FREQ=");
          char *countStr = strstr(value, "COUNT=");
          char *intervalStr = strstr(value, "INTERVAL=");

          if (countStr != NULL) {
            sscanf(countStr, "COUNT=%d%s", &calendarEvent->count, TEMP_CHARS);
          }
          if (intervalStr != NULL) {
            sscanf(intervalStr, "INTERVAL=%d%s", &calendarEvent->interval, TEMP_CHARS);
          }
          if (freqStr != NULL) {
            if (startsWith(freqStr, "FREQ=MINUTELY") != NULL) {
              calendarEvent->freq = CalendarRepeatFreq::MINUTELY;
              calendarEvent->repeat = 60;
            }
            if (startsWith(freqStr, "FREQ=HOURLY") != NULL) {
              calendarEvent->freq = CalendarRepeatFreq::HOURLY;
              calendarEvent->repeat = 60 * 60l;
            }
            if (startsWith(freqStr, "FREQ=DAILY") != NULL) {
              calendarEvent->freq = CalendarRepeatFreq::DAILY;
              calendarEvent->repeat = 60 * 60 * 24l;
            }
            if (startsWith(freqStr, "FREQ=WEEKLY") != NULL) {
              calendarEvent->freq = CalendarRepeatFreq::WEEKLY;
              calendarEvent->repeat = 60 * 60 * 24 * 7l;
            }
          }
          //Serial.printf("value=%s\n", value);
        }
      }
      if (!ignoreCurrentChar) {
        fileLine[fileLineIndex++] = c;
        if (fileLineIndex >= FILE_LINE_SIZE) {
          Serial.printf("ERROR: line++ access error!");
        }
      }

      if (c == '\n') {
        wasNewLine = true;
      } else {
        wasNewLine = false;
      }
    }
  }
  xSemaphoreGive(calendarEventsMutex);

  free(filePart->data);
  free(filePart);
  vTaskDelete(NULL);
}



void handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool isFinal) {
  Serial.printf("handleFileUploadTask\n");

  FilePart *filePart = new FilePart();
  strcpy(filePart->filename, filename.c_str());
  filePart->index = index;
  filePart->data = (uint8_t *)malloc(sizeof(uint8_t) * len);
  filePart->len = len;
  filePart->isFinal = isFinal;

  memcpy(filePart->data, data, len);

  Serial.printf("xTaskCreate\n");
  xTaskCreate(
    handleFileUploadTask,       // Function that should be called
    "Handle File Upload Task",  // Name of the task (for debugging)
    5000,                       // Stack size (bytes)
    filePart,                   // Parameter to pass
    index + 1,                  // Task priority
    NULL                        // Task handle
  );
}

char *getStringFromEnum(CalendarRepeatFreq freq) {
  switch (freq) {
    case CalendarRepeatFreq::DAILY: return "DAILY";
    case CalendarRepeatFreq::HOURLY: return "HOURLY";
    case CalendarRepeatFreq::MINUTELY: return "MINUTELY";
    case CalendarRepeatFreq::MONTHLY: return "MONTHLY";
    case CalendarRepeatFreq::SECONDLY: return "SECONDLY";
    case CalendarRepeatFreq::YEARLY: return "YEARLY";
    case CalendarRepeatFreq::WEEKLY: return "WEEKLY";
    default: return "-";
  }
}

void handleBroadcastEvents(AsyncWebServerRequest *request) {
  pingEventsTask();
  request->redirect("/");
}

void handleChangeCalendarEvent(AsyncWebServerRequest *request) {
  AsyncWebParameter *idParam = request->getParam("id", false, false);
  AsyncWebParameter *stateParam = request->getParam("active", false, false);
  String eventId = idParam->value();
  int state = stateParam->value().toInt();
  for (int i = 0; i < calendarEvents.getSize(); ++i) {
    CalendarEvent *event = calendarEvents.get(i);
    if (strcmp(event->uid, eventId.c_str()) == 0) {
      event->active = state ? true : false;
    }
  }
  pingEventsTask();
  request->redirect("/");
}

void handleRoot(AsyncWebServerRequest *request) {
  AsyncWebParameter *n = request->getParam("n", false, false);
  AsyncWebParameter *d = request->getParam("d", false, false);
  AsyncWebParameter *v = request->getParam("v", false, false);
  AsyncWebParameter *c = request->getParam("c", false, false);

  if (n != NULL && d != NULL && v != NULL && c != NULL) {
    String remoteNodeId = n->value();
    String remoteDeviceId = d->value();
    long remoteCaptcha = c->value().toInt();
    bool remoteDeviceValue = v->value().compareTo("true") ? false : true;


    Serial.printf("/%s/%s/%s\n", remoteNodeId, remoteDeviceId, remoteDeviceValue ? "true" : "false");

    if (remoteNodeId.compareTo(nodeId)) {
      DeviceEvent event;
      strcpy(event.nodeId, remoteNodeId.c_str());
      strcpy(event.deviceId, remoteDeviceId.c_str());
      event.value = remoteDeviceValue;
      event.captcha = remoteCaptcha;

      uint8_t shaResult[32];
      calculateHash(&event, sizeof(DeviceEvent), shaResult);

      // send packet
      LoRa.beginPacket();
      LoRa.write(MESSAGE_TYPE_CONTROL);
      LoRa.write((uint8_t *)&event, sizeof(DeviceEvent));
      LoRa.write(shaResult, 32);
      LoRa.endPacket();
      LoRa.receive();
      Serial.printf("SENT: %s\n", event.nodeId);

    } else {
      setLocalState(remoteDeviceId, remoteDeviceValue);
    }
  }

  String html = "";
  html += "<html><head><style>table, th, td {border: 1px solid;}</style></head><body>";
  html += "<h1>" + String(nodeId) + "</h1>";
  html += "<h2>Time</h2>";

  time_t time;
  if (haveTime) {

    struct tm timeinfo;
    getLocalTime(&timeinfo);
    timeinfo.tm_isdst = 0;
    time = mktime(&timeinfo);

    char timeStr[256];
    strftime(timeStr, sizeof(timeStr), "%A, %B %d %Y %H:%M:%S", &timeinfo);
    html += String(timeStr);
  } else {
    time = 0;
    html += "Time not synchronized";
  }

  html += "<h2>Nodes</h2>";
  html += "<table><tr><th>NodeId</th><th>Time</th><th>Captcha</th><th>Devices</th></tr>";

  for (int i = 0; i < nodeInfo.getSize(); ++i) {
    NodeInfo *node = nodeInfo.get(i);


    long c = node->captcha;

    String dshtml = "";
    for (int j = 0; j < node->devices->getSize(); ++j) {
      DeviceInfo *device = node->devices->get(j);
      Serial.printf("\tdevice(id=%s,value=%d)\n", device->deviceId, device->state);
      dshtml += String(device->deviceId) + "(" + (device->state ? "true" : "false") + ")[" + "<a href='/?n=" + String(node->nodeId) + "&d=" + String(device->deviceId) + "&v=" + (device->state ? "false" : "true") + "&c=" + String(node->captcha) + "'>switch</a>" + "]<br>";
    }

    struct tm *timeinfo;
    char timeStr[256];
    timeinfo = gmtime(&node->time);
    strftime(timeStr, 256, "%d/%m/%Y %H:%M:%S", timeinfo);

    html += "<tr><td>" + String(node->nodeId) + "</td><td>" + String(timeStr) + "</td><td>" + String(node->captcha) + "</td><td>" + dshtml + "</td></tr>";
  }
  html += "</table>";

  html += "<h2>Calendar</h2>";
  html += "<form method=\"post\" enctype=\"multipart/form-data\" action=\"/\">Upload a calendar:<br><input name=\"file\" id=\"file\" type=\"file\" accept=\".ics\"/><br><input type=\"submit\"/></form>";


  {
    html += "<h2>Schedulled Events</h2>";
    html += "<table>";
    html += "<tr><th>state</th><th>uid</th><th>node</th><th>device</th><th>start</th><th>end</th><th>freq</th><th>interval</th><th>repeat</th><th>stripStart</th><th>stripEnd</th><th>stripTime</th><th>active</th></tr>";
    int numberOfEvents = calendarEvents.getSize();
    for (int i = 0; i < numberOfEvents; ++i) {
      CalendarEvent *event = (CalendarEvent *)calendarEvents.get(i);

      struct tm *timeinfo;

      char startStr[256];
      char endStr[256];
      timeinfo = gmtime(&event->start);
      strftime(startStr, 256, "%d/%m/%Y %H:%M:%S", timeinfo);
      timeinfo = gmtime(&event->end);
      strftime(endStr, 256, "%d/%m/%Y %H:%M:%S", timeinfo);

      long loopLength = event->repeat * event->interval;
      long cyclesSinceStart = (time - event->start) / loopLength;

      time_t stripTime = (time - event->start) % loopLength;
      time_t stripStart = 0;
      time_t stripEnd = (event->end - event->start) % loopLength;



      html += "<tr><td>" + String(event->onGoing ? "RUNNING" : "WAITING") + "</td><td>" + String(event->uid) + "</td><td>" + String(event->nodeId)
              + "</td><td>" + String(event->deviceId) + "</td><td>" + String(startStr) + "</td><td>" + String(endStr) + "</td><td>" + String(getStringFromEnum(event->freq))
              + "</td><td>" + String(event->interval) + "</td><td>" + String(event->repeat)
              + "</td><td>" + String(stripStart) + "</td><td>" + String(stripEnd) + "</td><td>" + String(stripTime) + "</td><td>" + String(event->active ? "true" : "false") + "[<a href='/calendar?id=" + String(event->uid) + "&active=" + String(!event->active) + " '>switch</a>]</td></tr>";
    }
    html += "</table>";
    html += "<a href=\"/broadcast\"><button>Broadcast events</button></a>";
  }







  html += "</body></html>";
  request->send(200, "text/html", html.c_str());
}

void calculateHash(void *payload, size_t len, uint8_t result[32]) {
  size_t lenSecret = secret.length();
  char *payloadPlusSecret = (char *)malloc(sizeof(char) * (len + lenSecret));
  memcpy(payloadPlusSecret, payload, len);
  memcpy(payloadPlusSecret + len, secret.c_str(), lenSecret);


  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)payloadPlusSecret, len + lenSecret);
  mbedtls_sha256_finish(&ctx, result);
  mbedtls_sha256_free(&ctx);

  free(payloadPlusSecret);
}

void cleanCalendarEvents() {
  int numberOfEvents = calendarEvents.getSize();
  for (int i = 0; i < numberOfEvents; ++i) {
    CalendarEvent *event = (CalendarEvent *)calendarEvents.get(i);
    free(event);
  }
  calendarEvents.clear();
}

void hashToString(uint8_t hash[32], char str[64]) {
  for (int i = 0; i < 32; i++) {
    sprintf(str + i * 2, "%02x", (uint8_t)hash[i]);
  }
}


void onReceive(int packetSize) {
  // received a packet
  uint8_t *packet = (uint8_t *)malloc(sizeof(uint8_t) * (packetSize));
  // read packet
  for (int i = 0; i < packetSize; i++) {
    packet[i] = (uint8_t)LoRa.read();
  }
  Data *data = (Data *)malloc(sizeof(Data));
  data->packet = packet;
  data->packetSize = packetSize;

  xTaskCreate(
    messageTask,     // Function that should be called
    "Message Task",  // Name of the task (for debugging)
    10000,           // Stack size (bytes)
    data,            // Parameter to pass
    1,               // Task priority
    NULL             // Task handle
  );
}

NodeInfo *getNodeInfo(uint8_t *bytes, int length) {
  NodeInfo *nodeInfo = (NodeInfo *)malloc(sizeof(NodeInfo));
  memcpy(nodeInfo, bytes, sizeof(NodeInfo));
  Serial.printf("\tnode(id=%s,captcha=%d)\n", nodeInfo->nodeId, nodeInfo->captcha);
  nodeInfo->devices = new List<DeviceInfo *>();
  for (int i = sizeof(NodeInfo); i < length; i += sizeof(DeviceInfo)) {
    DeviceInfo *device = (DeviceInfo *)malloc(sizeof(DeviceInfo));
    memcpy(device, bytes + i, sizeof(DeviceInfo));
    Serial.printf("\tdevice(id=%s,value=%d)\n", device->deviceId, device->state);
    nodeInfo->devices->add(device);
  }

  return nodeInfo;
}

void addNodeInfo(NodeInfo *node) {
  for (int i = 0; i < nodeInfo.getSize(); ++i) {
    NodeInfo *n = nodeInfo.get(i);
    if (strcmp(node->nodeId, n->nodeId) == 0) {
      for (int j = 0; j < n->devices->getSize(); ++j) {
        free(n->devices->get(j));
      }
      free(n->devices);
      free(n);
      nodeInfo.remove(i--);
    }
  }
  nodeInfo.add(node);
}


void messageTask(void *parameter) {
  Data *data = (Data *)parameter;
  int packetSize = data->packetSize;
  uint8_t *packet = data->packet;
  free(data);

  if (packetSize > 33) {
    uint8_t type = packet[0];
    Serial.printf("\ttype='%d'\n", type);

    uint8_t messageSize = packetSize - 32 - 1;
    char *message = (char *)&packet[1];
    Serial.printf("\tmessage='%.*x'\n", messageSize, message);

    uint8_t hash[32];
    uint8_t expectedHash[32];
    char shaStr[64];

    memcpy(hash, &packet[packetSize - 32], sizeof(uint8_t) * 32);
    hashToString(hash, shaStr);
    Serial.printf("\thash='%.*s'\n", 64, shaStr);

    calculateHash(message, messageSize, expectedHash);
    hashToString(expectedHash, shaStr);
    Serial.printf("\texpectedHash='%.*s'\n", 64, shaStr);

    if (memcmp(hash, expectedHash, 32) == 0) {

      if (type == MESSAGE_TYPE_PING) {
        NodeInfo *pingMessage = getNodeInfo(packet + 1, packetSize - 1 - 32);
        addNodeInfo(pingMessage);

#ifndef SERVER
        if (!haveTime && pingMessage->time) {
          rtc.setTime(pingMessage->time, 0);
          haveTime = true;
        }
#endif



      } else if (type == MESSAGE_TYPE_CONTROL) {
        DeviceEvent *event = (DeviceEvent *)malloc(sizeof(DeviceEvent));
        memcpy(event, packet + 1, sizeof(DeviceEvent));

        if (strcmp(event->nodeId, nodeId) == 0) {
          if (event->captcha == captcha) {
            setLocalState(String(event->deviceId), event->value);
            captcha = esp_random();
          } else {
            Serial.println("ERROR: Failed captcha!");
          }
        } else {
          Serial.println("WARN: Wrong recepient!");
        }

      } else if (type == MESSAGE_TYPE_EVENT) {
        EventMessage message;

        CalendarEvent *event = (CalendarEvent *)malloc(sizeof(CalendarEvent));
        int index = packet[1];
        if (index == 0) {
          cleanCalendarEvents();
        }
        Serial.printf("LoRa Event %d\n", index);
        memcpy(&message, packet + 1, sizeof(EventMessage));
        memcpy(event, &message.event, sizeof(CalendarEvent));

        Serial.printf("\tEvent %s\n", event->uid);
        if (xSemaphoreTake(calendarEventsMutex, portMAX_DELAY)) {
          calendarEvents.add(event);
        }
        xSemaphoreGive(calendarEventsMutex);
      }
    } else {
      Serial.println("ERROR: Signature failed!");
    }
  }
  free(packet);
  vTaskDelete(NULL);
}

void pingEventsTask() {
  if (xSemaphoreTake(calendarEventsMutex, portMAX_DELAY)) {
    int numberOfEvents = calendarEvents.getSize();
    for (int i = 0; i < numberOfEvents; ++i) {
      CalendarEvent *event = (CalendarEvent *)calendarEvents.get(i);
      LoRa.beginPacket();
      LoRa.write(MESSAGE_TYPE_EVENT);
      EventMessage message;
      message.index = (uint8_t)i;
      message.event = *event;

      LoRa.write((uint8_t *)&message, sizeof(EventMessage));

      uint8_t shaResult[32];
      calculateHash(&message, sizeof(EventMessage), shaResult);

      LoRa.write(shaResult, 32);
      LoRa.endPacket();
    }
    xSemaphoreGive(calendarEventsMutex);
  }
}

void pingTask(void *parameter) {
  char message[256];

  for (;;) {  // infinite loop
    uint8_t messageLength = sizeof(NodeInfo) + 2 * sizeof(DeviceInfo);
    uint8_t *messageBytes = (uint8_t *)malloc(sizeof(uint8_t) * messageLength);

    NodeInfo pingMessage;
    DeviceInfo relay1Message;
    DeviceInfo relay2Message;

    strcpy(pingMessage.nodeId, nodeId);
    strcpy(relay1Message.deviceId, "r1");
    strcpy(relay2Message.deviceId, "r2");
    relay1Message.state = relay1Status;
    relay2Message.state = relay2Status;
    pingMessage.captcha = captcha;

    if (haveTime) {
      pingMessage.time = rtc.getEpoch();
    }

    memcpy(messageBytes, &pingMessage, sizeof(NodeInfo));
    memcpy(messageBytes + sizeof(NodeInfo), &relay1Message, sizeof(DeviceInfo));
    memcpy(messageBytes + sizeof(NodeInfo) + sizeof(DeviceInfo), &relay2Message, sizeof(DeviceInfo));

    Serial.printf("addNodeInfo()\n");

    addNodeInfo(getNodeInfo(messageBytes, messageLength));  //TODO check repeated pings
    //Serial.printf("\tmessage='%jd'\n", messageBytes+32);

    uint8_t shaResult[32];
    calculateHash(messageBytes, messageLength, shaResult);

    // send packet
    LoRa.beginPacket();
    LoRa.write(MESSAGE_TYPE_PING);
    LoRa.write(messageBytes, messageLength);
    LoRa.write(shaResult, 32);
    LoRa.endPacket();
#ifdef SERVER
    pingEventsTask();
#endif
    LoRa.receive();

    free(messageBytes);
    // Pause the task again for 500ms
    vTaskDelay(10000 / portTICK_PERIOD_MS);
  }
}

void wifiTask(void *parameter) {

// WiFi Setup
#ifdef SERVER
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  myIP = WiFi.localIP().toString(false);
  // Init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  rtc.setTimeStruct(timeinfo);
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  haveTime = true;
#else
  // Remove the password parameter, if you want the AP (Access Point) to be open
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  myIP = IP.toString();
  Serial.println(IP);
#endif

  vTaskDelete(NULL);
}


void eventSchedulerTask(void *parameter) {
  for (;;) {  // infinite loop
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    timeinfo.tm_isdst = 0;

    time_t time = mktime(&timeinfo);
    Serial.printf("eventSchedulerTask %jd\n", time);

    if (xSemaphoreTake(calendarEventsMutex, portMAX_DELAY)) {
      int numberOfEvents = calendarEvents.getSize();

      for (int i = 0; i < numberOfEvents; ++i) {
        CalendarEvent *event = (CalendarEvent *)calendarEvents.get(i);
        Serial.printf("Handling event (uid=%s,node=%s,device=%s,start=%jd,end=%jd,freq=%d,repeat=%jd,count=%d,interval=%d)\n", event->uid, event->nodeId, event->deviceId, event->start, event->end, event->freq, event->repeat, event->count, event->interval);
        time_t loopLength = event->repeat * event->interval;
        time_t cyclesSinceStart = (time - event->start) / loopLength;

        time_t stripTime = (time - event->start) % loopLength;
        time_t stripStart = 0;
        time_t stripEnd = (event->end - event->start) % loopLength;
        //Serial.printf("strip(start=%jd,end=%jd,time=%jd,len=%ld,cycles=%ld)\n", stripStart, stripEnd, stripTime, loopLength, cyclesSinceStart);

        bool evaluation = (stripStart <= stripTime && stripTime < stripEnd) && (event->count == 0 || cyclesSinceStart < event->count);
        event->onGoing = evaluation;
        //Serial.printf("location(nodeId=%s,deviceId=%s)\n", event->nodeId, event->deviceId);
      }



      int numberOfNodes = nodeInfo.getSize();
      for (int i = 0; i < numberOfNodes; ++i) {
        NodeInfo *node = (NodeInfo *)nodeInfo.get(i);
        if (strcmp(nodeId, node->nodeId) == 0) {

          int numberOfDevices = node->devices->getSize();
          int numberOfEvents = calendarEvents.getSize();

          for (int j = 0; j < numberOfDevices; ++j) {
            DeviceInfo *device = node->devices->get(j);
            int triggers = 0;
            int events = 0;
            for (int k = 0; k < numberOfEvents; ++k) {
              CalendarEvent *calendarEvent = calendarEvents.get(k);
              if (calendarEvent->active) {
                ++events;
                if (calendarEvent->onGoing) {
                  ++triggers;
                }
              }
            }
            if (events) {
              setLocalState(String(device->deviceId), triggers > 0);
            }
          }
        }
      }



      xSemaphoreGive(calendarEventsMutex);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}


void setup() {

  Serial.begin(115200);
  while (!Serial)
    ;


  Serial.println("init ok");
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.clearDisplay();
  display.printf("setup...\n");
  display.display();


  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DI0);
  if (!LoRa.begin(868E6)) {
    Serial.println("Starting LoRa failed!");
    // TODO: print on display insted
    while (1)
      ;
  }
  //LoRa.onReceive(cbk);
  //  LoRa.receive();

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  setLocalState("r1", false);
  setLocalState("r2", false);

  LoRa.onReceive(onReceive);
  LoRa.receive();

  xTaskCreate(
    pingTask,     // Function that should be called
    "Ping Task",  // Name of the task (for debugging)
    10000,        // Stack size (bytes)
    NULL,         // Parameter to pass
    1,            // Task priority
    NULL          // Task handle
  );

  xTaskCreate(
    wifiTask,     // Function that should be called
    "Wifi Task",  // Name of the task (for debugging)
    10000,        // Stack size (bytes)
    NULL,         // Parameter to pass
    1,            // Task priority
    NULL          // Task handle
  );

  xTaskCreate(
    eventSchedulerTask,      // Function that should be called
    "Event Scheduler Task",  // Name of the task (for debugging)
    10000,                   // Stack size (bytes)
    NULL,                    // Parameter to pass
    1,                       // Task priority
    NULL                     // Task handle
  );

  server.on("/", HTTP_GET, handleRoot);
  server.on("/broadcast", HTTP_GET, handleBroadcastEvents);
  server.on("/calendar", HTTP_GET, handleChangeCalendarEvent);
  server.on(
    "/", HTTP_POST, [](AsyncWebServerRequest *request) {
      pingEventsTask();
      request->redirect("/");
    },
    handleFileUpload);
  server.begin();
}

void loop() {
  struct tm timeinfo;
  char timeStr[256];
  getLocalTime(&timeinfo);
  strftime(timeStr, sizeof(timeStr), "%d/%m/%Y %H:%M:%S", &timeinfo);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.printf("%s\n%s\n%s\n%s\n", "LoRa Control v1.1", nodeId, timeStr, myIP.c_str());
  display.display();

  delay(1000);  // wait for a second
}