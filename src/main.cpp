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
void ICACHE_RAM_ATTR ch1_edge_counter_ISR(void);

char wifiSsid[1024]      = "";  
char wifiPassword[1024]  = "";
char mqttServer[1024]    = ""; 
int  mqttPort            = 0; 
char mqttUser[1024]      = "";
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

#define  PIN_CH1 D5 // 200Hz pulses while all channels lit, low active while channel 1 selected
#define  PIN_CH  D0
#define  PIN_UP  D6
#define  PIN_MY  D7
#define  PIN_DWN D8

typedef struct somfy_command {
  eSomfy_Cmd somfy_command_requested = s_NONE;
  int somfy_channel_requested = -1;
} sSomfy_Command;

#define IMPLEMENTATION FIFO	
cppQueue qSomfyCommands(sizeof(somfy_command), 5, IMPLEMENTATION);

// match state object
MatchState matchstate;

// ==================================== Globals ==========================================

unsigned int number_of_edges = 0;

unsigned long current_millis = 0;
unsigned long prev_millis = 0;

int mqtt_status = 30 * 1000; //30s
int dig_toggle = 250;

int channel_selected = -1;
int channel_requested = -1;
int number_of_triggers = 0;
eSomfy_Cmd command_last_executed = s_NONE;
somfy_command* request_pending = NULL;

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

void trigger_pin_for_ms(uint8_t pin, unsigned long t_delay) {
  digitalWrite(pin, !digitalRead(pin));
  delay(t_delay);
  digitalWrite(pin, !digitalRead(pin));
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
  delay(100);
  cfile.close();
  
  // DIO -------------------------------------------------------------------------------
  pinMode(PIN_CH,  OUTPUT); 
  pinMode(PIN_UP,  OUTPUT); 
  pinMode(PIN_MY,  OUTPUT); 
  pinMode(PIN_DWN, OUTPUT); 

  //attach to interrupt for pulse detection
  attachInterrupt(digitalPinToInterrupt(PIN_CH1), ch1_edge_counter_ISR, FALLING);
  pinMode(PIN_CH1, INPUT); 
  
  digitalWrite(PIN_CH, HIGH);
  digitalWrite(PIN_UP, HIGH);
  digitalWrite(PIN_MY, HIGH);
  digitalWrite(PIN_DWN, HIGH);
  
  // Wifi ------------------------------------------------------------------------------
  Serial.begin(115200);
  delay(100);
  Serial.println("Connecting to WiFi");

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.setOutputPower(10.0);
  WiFi.setPhyMode(WIFI_PHY_MODE_11N);
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  
  WiFi.begin(wifiSsid, wifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("...Connecting to WiFi");
    delay(1000);
  }  
  Serial.println("Connected to WiFi");
  
  delay(100);
  
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
  mqtt_client.subscribe("shades/terasse/cmd/#");
  Serial.println("-- End of Setup --");
}

void mqtt_callback (char* topic, byte* payload, unsigned int length) {
  char c_payload[length + 1];
  Serial.print("MQTT_callback -> Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("MQTT_callback -> Message: ");
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
      sSomfy_Command* req = (sSomfy_Command*) malloc(sizeof(sSomfy_Command));
      req->somfy_command_requested = somfy_cmd_ctoe(c_payload);
      req->somfy_channel_requested = recv_channel;
      Serial.printf("MQTT_callback -> Channel: %d\n", req->somfy_channel_requested);
      Serial.printf("MQTT_callback -> Command: %s\n", somfy_cmd_etoc(req->somfy_command_requested));
      qSomfyCommands.push(&req);
    }
  }
}

void ch1_edge_counter_ISR (void) {
  number_of_edges = number_of_edges + 1;
  return;
}


// ==================================== LOOP ==================================== 
void loop() {
  // put your main code here, to run repeatedly:
  mqtt_client.loop();
  current_millis = millis();

  if ((int)(current_millis - prev_millis) >= mqtt_status) {
    char mqtt_message[80];
    // //last channel selected
    snprintf(mqtt_message, 80, "{ channel: %d }\n", channel_selected);
    mqtt_client.publish("shades/terasse/state/CH_SELECT", mqtt_message, 80);
    //last command executed
    snprintf(mqtt_message, 80, "{ command: %s }\n", somfy_cmd_etoc(command_last_executed));
    mqtt_client.publish("shades/terasse/state/COMMAND_LAST", mqtt_message, 80);

    prev_millis = current_millis;
  }

  if (qSomfyCommands.getCount() > 0) {
    //queue information
    Serial.printf("loop -> Queue count: %d\n", qSomfyCommands.getCount());
    Serial.printf("loop -> Queue full : %d\n", qSomfyCommands.isFull());
    if (request_pending == NULL) {
      //get next command
      qSomfyCommands.pop(&request_pending);
      Serial.println("loop -> Queue Popped.");
      //store information for mqtt
      command_last_executed = request_pending->somfy_command_requested;
      channel_requested = request_pending->somfy_channel_requested;
    } else {
      Serial.println("loop -> Queue not popped.");
    }
  }

  //we have an active request to fullfill
  if (request_pending != NULL) {
    if (request_pending->somfy_channel_requested != channel_selected) {
      //detect and/or switch channels
      if (channel_selected == -1) {
        //toggle CH 
        trigger_pin_for_ms(PIN_CH, dig_toggle);
        //check CH1 pulses -> 200Hz with 2,5ms Pulses, Wait time would be like 50ms max and more than 10 pulses
        delay(200);
        Serial.printf("loop -> # of edges %d\n", number_of_edges);
        if (number_of_edges > 0) {
          if (number_of_edges > 20) {
            channel_selected = 0;
            Serial.println("loop -> more than 20 falling edges found. Assuming 20Hz toggle, channel 0.");
          }
          else if (number_of_edges == 1) {
            channel_selected = 1;
            Serial.println("loop -> found single falling edge. CH1");
          }
          else {
            Serial.println("loop -> no edges found.");
            //this would fall through by intention to trigger CH select again.
          }
          number_of_edges = 0;
        }
      } else {
        if (number_of_triggers == 0) {
          int distance = request_pending->somfy_channel_requested - channel_selected;
          if (distance < 0) {
            //e.g. if we're at 4 and need to go to 2, then we have -2, which is 3 steps as 0 and 1 are inbetween
            distance += NUMBER_SOMFY_CHANNELS;
          }
          number_of_triggers = distance;
        } else {
          trigger_pin_for_ms(PIN_CH, 100);
          delay(250);
          number_of_triggers--;
        }
      }
      
      if (channel_selected != -1) {
        Serial.printf("loop -> Channels switched to %d\n", channel_selected);
      }
    }

    if (request_pending->somfy_channel_requested == channel_selected) {
      //trigger command for channel as it is set
      uint8_t pin = PIN_CH;
      int t_delay = dig_toggle;
      Serial.println("loop -> Toggle right pin for button.");
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
      trigger_pin_for_ms(pin, t_delay);
      Serial.println("loop -> Toggled.");
      //clear active request as done
      free(request_pending);
      delay(10);
      request_pending = NULL;
      Serial.println("loop -> Request cleared.");
      Serial.printf("Queue size: %d\n", qSomfyCommands.getCount());
    }
  }
}