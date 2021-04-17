#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266Wifi.h>
#endif
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <Regexp.h>
#include <cppQueue.h>

// generic setup

void mqtt_callback (char* topic, byte* payload, unsigned int length); 

char wifiSsid[1024]  = "";  
char wifiPassword[1024]  = "";
char mqttServer[1024]  = ""; 
int   mqttPort     = 0; 
char mqttUser[1024]  = "";
char mqttPassword[1024]  = "";

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

//somfy parts

#define FOREACH_CMD(SCOMMAND) \
        SCOMMAND(s_NONE) \
        SCOMMAND(s_UP)   \
        SCOMMAND(s_MY)   \
        SCOMMAND(s_STOP) \
        SCOMMAND(s_DOWN) \
        SCOMMAND(s_Last)

#define GENERATE_ENUM(ENUM) ENUM,
#define GENERATE_STRING(STRING) #STRING,

const char* SOMFY_CMD_STRING[] = {
  FOREACH_CMD(GENERATE_STRING)
};

typedef enum somfy_cmd {
  FOREACH_CMD(GENERATE_ENUM)
} eSomfy_Cmd;


#define NUMBER_SOMFY_CHANNELS 4

#define  PIN_CH1 A0 // - 3.0V no led, 1.4V all led, 0V CH1 led
#define  PIN_CH  D0
#define  PIN_UP  D5
#define  PIN_MY  D6
#define  PIN_DWN D7

typedef struct somfy_command {
  eSomfy_Cmd somfy_command_requested = s_NONE;
  int somfy_channel_requested = -1;
} sSomfy_Command;

#define IMPLEMENTATION FIFO	
cppQueue qSomfyCommands(sizeof(somfy_command), 5, IMPLEMENTATION);

// match state object
MatchState matchstate;

// ==================================== CODE ==================================== 

const char* somfy_cmd_etoc(eSomfy_Cmd command) {
  return SOMFY_CMD_STRING[command];
}

eSomfy_Cmd somfy_cmd_ctoe(char* command) {
  eSomfy_Cmd res = s_NONE;
  if (!strncmp(command, "up", 2)) {
    res = s_UP;
  } else if (!strncmp(command, "down", 4)) {
    res = s_DOWN;
  } else if (!strncmp(command, "my", 2)) {
    res = s_MY;
  } else if (!strncmp(command, "stop", 4)) {
    res = s_STOP;
  }
  return res;
}

// ==================================== SETUP ==================================== 

void setup() {
  Serial.begin(115200);
  //read config
  if (!LittleFS.begin()) {
    Serial.println("LITTLEFS Mount Failed");
  }
  File cfile = LittleFS.open("/config.json", "r");
  if (cfile) {
    String config_data;
    while (cfile.available()) {
      config_data += char(cfile.read());
    }
    Serial.print(config_data);   
    DynamicJsonDocument cjson(config_data.length());
    deserializeJson(cjson, config_data);

    const char* c_wifiSsid     = cjson["wifiSsid"];  
    const char* c_wifiPassword = cjson["wifiPassword"];
    const char* c_mqttServer   = cjson["mqttServer"]; 
    const char* c_mqttPort     = cjson["mqttPort"]; 
    const char* c_mqttUser     = cjson["mqttUser"]; 
    const char* c_mqttPassword = cjson["mqttPassword"];

    Serial.print(c_wifiSsid);

    if (strlen(c_wifiSsid)) { 
      sprintf(wifiSsid, "%s", c_wifiSsid); 
    }
    if (strlen(c_wifiPassword)) { 
      sprintf(wifiPassword, "%s", c_wifiPassword); 
      }
    if (strlen(c_mqttServer)) { 
      sprintf(mqttServer, "%s", c_mqttServer); 
    }
    if (strlen(c_mqttPort)) { 
      mqttPort = atoi(c_mqttPort);
    }
    if (strlen(c_mqttUser)) { 
      sprintf(mqttUser, "%s", c_mqttUser);
    }
    if (strlen(c_mqttPassword)) { 
      sprintf(mqttPassword, "%s", c_mqttPassword);
    }

  } else {
    Serial.print("Config File missing.");
    for (;;)
      delay(1);
  }
  cfile.close();
  
  // DIO -------------------------------------------------------------------------------
  // pinMode(PIN_CH1, ANALOG); 
  pinMode(PIN_CH,  OUTPUT); 
  pinMode(PIN_UP,  OUTPUT); 
  pinMode(PIN_MY,  OUTPUT); 
  pinMode(PIN_DWN, OUTPUT); 

  digitalWrite(PIN_CH, HIGH);
  digitalWrite(PIN_UP, HIGH);
  digitalWrite(PIN_MY, HIGH);
  digitalWrite(PIN_DWN, HIGH);
  
  // Wifi ------------------------------------------------------------------------------
  Serial.begin(115200);
  delay(1000);
  Serial.println("Connecting to WiFi");

  WiFi.disconnect();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(10.0);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  WiFi.persistent(false);
  
  WiFi.begin(wifiSsid, wifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("...Connecting to WiFi");
    delay(1000);
  }  
  Serial.println("Connected to WiFi");

  // MQTT ------------------------------------------------------------------------------
  mqtt_client.setServer(mqttServer, mqttPort);
  mqtt_client.setCallback(mqtt_callback);
  
  while (!mqtt_client.connected()) {
    Serial.println("...Connecting to MQTT");
    if (mqtt_client.connect("somfyIO", mqttUser, mqttPassword )) {
      Serial.println("Connected to MQTT");
    } else {
      Serial.print("Failed connecting MQTT with state: ");
      Serial.print(mqtt_client.state());
      delay(2000);
    }
  }

  mqtt_client.publish("shades/terasse/cmd/channel99", "Hello World.");
  mqtt_client.subscribe("shades/terasse/cmd/#");// here is where you later add a wildcard
}

void mqtt_callback (char* topic, byte* payload, unsigned int length) {
  char* c_payload = (char*) malloc(length + 1);
  Serial.print("MQTT_callback -> Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("MQTT_callback -> Message:");
  for (unsigned int i = 0; i < length; i++) {
    c_payload[i] = (char)payload[i];
  }
  c_payload[length] = '\0';
  Serial.print(c_payload);
  Serial.println();

  matchstate.Target(topic);
  char result = matchstate.Match ("/cmd/channel%d");
  if (result > 0) {
    int r_start = matchstate.MatchStart;
    int r_len   = matchstate.MatchLength;
    int recv_channel = (int)(topic[r_start + r_len - 1] - 0x30);

    if (recv_channel >= 0 && recv_channel <= NUMBER_SOMFY_CHANNELS) {
      sSomfy_Command req;
      req.somfy_command_requested = somfy_cmd_ctoe(c_payload);
      req.somfy_channel_requested= recv_channel;
      Serial.printf("MQTT_callback -> Channel: %d\n", req.somfy_channel_requested);
      Serial.printf("MQTT_callback -> Command: %s\n", somfy_cmd_etoc(req.somfy_command_requested));
      qSomfyCommands.push(&req);
    }
  }
  free(c_payload);
}

unsigned long current_millis = 0;
unsigned long prev_millis = 0;

int mqtt_status = 30 * 1000; //30s
int dig_toggle = 100;

int val_ch1 = 0;
float val_ch1_voltage = 0.0;

int channel_selected = -1;
int channel_requested = -1;
eSomfy_Cmd command_last_executed = s_NONE;
somfy_command* request_pending = NULL;

// ==================================== LOOP ==================================== 

void loop() {
  // put your main code here, to run repeatedly:
  mqtt_client.loop();
  WiFi.status();
  current_millis = millis();
  yield();

  val_ch1 = analogRead(PIN_CH1);
  val_ch1_voltage = (float)val_ch1 * 3300 / 1042; //3.3V is 1024

  if ((int)(current_millis - prev_millis) >= mqtt_status) {
    char mqtt_message[80];
    //analog update
    snprintf(mqtt_message, 80, "{ value: %.0f, unit: mV }\n", val_ch1_voltage);
    mqtt_client.publish("shades/terasse/state/CH1", mqtt_message, 80);
    // //last channel selected
    snprintf(mqtt_message, 80, "{ channel: %d }\n", channel_selected);
    mqtt_client.publish("shades/terasse/state/CH_SELECT", mqtt_message, 80);
    //last command executed
    snprintf(mqtt_message, 80, "{ command: %s }\n", somfy_cmd_etoc(command_last_executed));
    mqtt_client.publish("shades/terasse/state/COMMAND_LAST", mqtt_message, 80);

    prev_millis = current_millis;
  }

  yield();
  if (qSomfyCommands.getCount() > 0) {
    //queue information
    Serial.printf("loop -> Queue count: %d\n", qSomfyCommands.getCount());
    Serial.printf("loop -> Queue full : %d\n", qSomfyCommands.isFull());
    if (request_pending == NULL) {
      //get next command
      qSomfyCommands.pop(&request_pending);
      //store information for mqtt
      command_last_executed = request_pending->somfy_command_requested;
      channel_requested = request_pending->somfy_channel_requested;
    }
  }

  //we have an active request to fullfill
  if (request_pending != NULL) {
    if (request_pending->somfy_channel_requested != channel_selected) {
      //TODO: detect and switch channels
      channel_selected = request_pending->somfy_channel_requested;
    }
    if (request_pending->somfy_command_requested == channel_selected) {
      //trigger command for channel as it is set
      uint8_t pin = PIN_CH;
      int t_delay = dig_toggle;
      
      switch (request_pending->somfy_command_requested)
      {
      case s_UP:
        pin = PIN_UP;
        break;
      case s_MY:
        t_delay = 500;
      case s_STOP:
        pin = PIN_MY;
        break;
      case s_DOWN:
        pin = PIN_DWN;
        break;
      default:
        break;
      }
      digitalWrite(pin, LOW);
      delay(t_delay);
      digitalWrite(pin, HIGH);
      //clear active request as done
      request_pending = NULL;
    }
  }
}