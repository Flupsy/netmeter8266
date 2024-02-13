#include <ESP8266WiFi.h>
#include <Pinger.h>
#include <WiFiUDP.h>
#include <NTPClient.h>
#include "TM1637DisplayAlnum.h"
#include "ArduinoSNMP.h"
#include "netmeter.h"

#define DEBUG

const int MODE_BUTTON = D1;

const int LED_UPDATE_INTERVAL = 500;
const int GENERAL_TIMEOUT     = 10000;
const int HTTP_TIMEOUT        = 1000;
const int HTTP_END_DELAY      = 500;
const int MAX_IFTABLE_NUMBER  = 24;

// NTP stuff
WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP, ntp_server, 0);

// Webserver stuff
WiFiServer server(80);            // run a 'webserver' on this port
const char *uri_index = "/";
const char *uri_mode = "/mode";

// Limits
const int ping_warn       = 50;
const int ping_critical   = 1000;

// Last time things happened
unsigned long last_comm = 0, last_ping = 0;

enum states {
  STATE_CONNECTING,
  STATE_IFTABLE,
  STATE_NO_IF,
  STATE_NO_COMM,
  STATE_NO_WIFI,
  STATE_NO_INTERNET,
  STATE_CRITICAL,
  STATE_WARNING,
  STATE_OK,
} current_state;

void mode_stats(void);
void mode_timedate(void);
void mode_swear(void);
void mode_addresses(void);

// Display modes
void (*mode_functions[])(void) = {
  mode_stats,
  mode_timedate,
  mode_swear,
  mode_addresses,

  NULL,
};

int current_mode = 0;
int ifnum = -1;        // determined during STATE_IFTABLE

// Variables for display modes
int ping_time, traffic_up, traffic_down;
uint32_t rx_bytes_last = 0, tx_bytes_last = 0;
unsigned long rx_bytes_when = 0, tx_bytes_when = 0;

const word local_port       = 16309;
const int snmp_version      = 0;

// Append ifnum to each of these
const char *ifdescr_oid     = "1.3.6.1.2.1.2.2.1.2";
const char *tx_oid          = "1.3.6.1.2.1.2.2.1.16";
const char *rx_oid          = "1.3.6.1.2.1.2.2.1.10";

Pinger pinger;

TM1637DisplayAlnum display_ping(D2, D3);
TM1637DisplayAlnum display_rx(D4, D5);
TM1637DisplayAlnum display_tx(D6, D7);

TM1637DisplayAlnum *displays[] = {
  &display_ping,
  &display_tx,
  &display_rx,
};

const uint8_t display_brightness = 0x07;

const char *msg_init[] = { "----", "init", "----" };
const char *msg_iftable[] = { "----", "intf", "----" };
const char *msg_no_if[] = { "cant", "find", "intf" };
const char *msg_no_comm[] = { "SHIT", " NO ", "NET " };
const char *msg_no_internet[] = { "PISS", " NO ", "PING" };

#ifdef DEBUG
void debug(const char *format, ...) {
  static char buf[128];
  va_list args;
  
  va_start(args, format);
  vsprintf(buf, format, args);
  va_end(args);

  Serial.print(millis()); Serial.print(": ");
  Serial.println(buf);
}

void debug(String str) {
  Serial.print(millis()); Serial.print(": ");
  Serial.println(str);
}
#else
#define debug
#endif


bool led_update_is_due(void) {
  static unsigned long last_led_update = 0;
  unsigned long now;

  now = millis();

  if(now - last_led_update > LED_UPDATE_INTERVAL) {
    last_led_update = now;
    return true;
  }

  return false;
}


void show_message(const char *msg[]) {
  for(int i=0; i<3; i++) {
    displays[i]->showAlnum(msg[i]);
  }
}


void clear_displays(void) {
  for(int i=0; i<3; i++) {
    displays[i]->clear();
  }
}

void mode_stats(void) {
  static bool flash;
  static enum states last_state;
  static int last_ping_time, last_traffic_up, last_traffic_down;
  bool update_ping = false, update_traffic_up = false, update_traffic_down = false;

  if(last_state != current_state) {
    flash = true;
    last_state = current_state;
  }

  if(led_update_is_due()) {
    if(ping_time != last_ping_time) {
      last_ping_time = ping_time;
      update_ping = true;
    }
  
    if(traffic_up != last_traffic_up) {
      last_traffic_up = traffic_up;
      update_traffic_up = true;
    }
  
    if(traffic_down != last_traffic_down) {
      last_traffic_down = traffic_down;
      update_traffic_down = true;
    }
  
    switch(current_state) {
      case STATE_CONNECTING:
        if(flash) {
          show_message(msg_init);
        } else {
          clear_displays();
        }

        break;
        
      case STATE_IFTABLE:
        if(flash) {
          show_message(msg_iftable);
        } else {
          clear_displays();
        }

        break;

      case STATE_NO_COMM:
        if(flash) {
          show_message(msg_no_comm);
        } else {
          display_ping.clear();
        }

        break;

      case STATE_NO_IF:
        if(flash) {
          show_message(msg_no_if);
        } else {
          display_ping.clear();
        }

        break;

      case STATE_NO_INTERNET:
        if(flash) {
          show_message(msg_no_internet);
        } else {
          display_ping.clear();
        }

        break;

      case STATE_CRITICAL:
        if(flash) {
          display_ping.showNumberDec(ping_time);
        } else {
          display_ping.clear();
        }

        break;

      case STATE_WARNING:
        if(flash) {
          display_ping.showNumberDec(ping_time);
        } else {
          display_ping.clear();
        }
        
        break;
        
      case STATE_OK:
        if(update_ping) {
          display_ping.showNumberDecEx(ping_time);
        }
        if(update_traffic_up) {
          display_tx.showNumberDecEx((traffic_up >= 10000) ? -traffic_up : traffic_up, (traffic_up > 99 && traffic_up < 100000) ? 0b11100000 : 0);
        }
        if(update_traffic_down) {
          display_rx.showNumberDecEx((traffic_down >= 10000)? -traffic_down : traffic_down, (traffic_down > 99 && traffic_down < 100000) ? 0b11100000 : 0);
        }

        break;
        
      default:
        //debug("INVALID STATE");
        break;
    }

    flash = ! flash;
  }
}


void new_state(const states new_state) {
  if(new_state != current_state) {
    debug("*** state change: %d -> %d", current_state, new_state);
    current_state = new_state;
    (void) led_update_is_due();
  }
}


void ping_setup(void) {
  pinger.OnReceive([](const PingerResponse& response) {
    if(response.ReceivedResponse) {
      last_ping = millis();
      debug("ping: %d ms",response.ResponseTime);
      ping_time = response.ResponseTime;
    } else {
      ping_time = 9999;
    }
    return true;
  });
}


void send_ping(void) {
  pinger.Ping(ping_target, 1);
}


void setup() {
#ifdef DEBUG
  Serial.begin(115200);
  debug("Carousel begins.");
  delay(1000);
#endif

  pinMode(MODE_BUTTON, INPUT_PULLUP);

  display_ping.setBrightness(display_brightness);
  display_tx.setBrightness(display_brightness);
  display_rx.setBrightness(display_brightness);
  
  new_state(STATE_CONNECTING);

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_network_name, wifi_network_password);

  ping_setup();
}


void send_snmp(const char *oid_prefix, int num) {
  SNMP_PDU pdu;
  SNMP_VALUE value;
  char oid[128];

  sprintf(oid, "%s.%d", oid_prefix, num);

  pdu.clear();

  pdu.type = SNMP_PDU_GET;
  pdu.version = 0;
  pdu.requestId = rand();
  debug("sending requestId %lu", pdu.requestId);
  pdu.error = SNMP_ERR_NO_ERROR;
  pdu.errorIndex = 0;

  value.OID.clear();
  value.clear();
  value.OID.fromString(oid);
  value.encode(SNMP_SYNTAX_NULL);
  pdu.add_data(&value);

  //debug(" -- sending a PDU");

  SNMP.send_message(&pdu, router, 161);
}


bool send_snmp_iftable(void) {
  if(ifnum == MAX_IFTABLE_NUMBER) {
    return false;
  }

  ifnum++;
  
  debug("send_snmp_iftable(): trying interface %d", ifnum);
  send_snmp(ifdescr_oid, ifnum);
  display_rx.showNumberDecEx(ifnum, 0b11100000);

  return true;
}


int new_mode(void) {
  current_mode++;
  if(! mode_functions[current_mode]) {
    current_mode = 0;
  }

  display_ping.clear();
  display_rx.clear();
  display_tx.clear();
  
  debug("*** new mode %d", current_mode);
  return current_mode;
}


void mode_timedate(void) {
  if(led_update_is_due()) {
    display_ping.showNumberDec(ntp.getHours(), true, 2, 2);
    display_tx.showNumberDec(ntp.getMinutes(), true, 2, 2);
    display_rx.showNumberDec(ntp.getSeconds(), true, 2, 2);
  }
}


void mode_swear(void) {
  static bool flash;
  static int msg_num;
  static const char **msg = msg_no_comm;

  if(led_update_is_due()) {
    if(flash) {
      if(msg_num == 0) {
        msg = msg_no_comm;
        msg_num = 1;  
      } else {
        msg = msg_no_internet;
        msg_num = 0;
      }
      
      display_ping.showAlnum(msg[0]);
      display_tx.clear();
      display_rx.clear();
    } else {
      display_ping.clear();
      display_tx.showAlnum(msg[1]);
      display_rx.showAlnum(msg[2]);
    }

    flash = ! flash;
  }
}


void mode_addresses(void) {
  static int octet, which;

  if(! led_update_is_due()) {
    return;
  }

  display_tx.showNumberDec(octet + 1);
  
  switch(which) {
    case 0:
      display_ping.showAlnum("Addr");
      display_rx.showNumberDec(WiFi.localIP()[octet]);
      break;

    case 1:
      display_ping.showAlnum("Netm");
      display_rx.showNumberDec(WiFi.subnetMask()[octet]);
      break;

    case 2:
      display_ping.showAlnum("Gate");
      display_rx.showNumberDec(WiFi.gatewayIP()[octet]);
      break;

    case 3:
      display_ping.showAlnum("Rout");
      display_rx.showNumberDec(router[octet]);
      break;

    case 4:
      display_ping.showAlnum("PING");
      display_rx.showNumberDec(ping_target[octet]);
      break;
  }

  octet++;
  if(octet > 3) {
    which++;
    if(which > 4) {
      which = 0;
    }
    octet = 0;
  }  
}


bool recv_snmp(void) {
  SNMP_PDU pdu;
  SNMP_API_STAT_CODES snmp_status;
  char oid[128], str[128];
  int i;
  uint32_t data;

  debug("recv_snmp() called");

  snmp_status = SNMP.requestPdu(&pdu, NULL, 0);
  if(snmp_status != SNMP_API_STAT_SUCCESS) {
    debug("parse failed!");
    return false;
  }

  if(pdu.error != SNMP_ERR_NO_ERROR) {
    debug("PDU indicated agent reported an error!");
    return false;
  }
  
  if(pdu.type != SNMP_PDU_RESPONSE) {
    debug("PDU isn't a response!");
    return false;
  }

  oid[0]='\0';
  pdu.value.OID.toString(oid, sizeof(oid)-1);

  switch(current_state) {
    case STATE_OK:
      if(strncmp(tx_oid, oid, strlen(tx_oid))==0) {
        pdu.value.decode(&data);
        debug("  tx_oid response received");
        if(tx_bytes_last) {
          traffic_up = (data - tx_bytes_last) * 8 / 10000 / ((millis() - tx_bytes_when) / 1000);
          debug("    traffic_up: %lu - %lu = %lu (%lu in %lums)", data, tx_bytes_last, traffic_up, data-tx_bytes_last, millis()-tx_bytes_when);
        }
        tx_bytes_last = data;
        tx_bytes_when = millis();
      } else if(strncmp(rx_oid, oid, strlen(rx_oid))==0) {
        pdu.value.decode(&data);
        debug("  rx_oid response received");
        if(rx_bytes_last) {
          traffic_down = (data - rx_bytes_last) * 8 / 10000 / ((millis() - rx_bytes_when) / 1000);
          debug("    traffic_down: %lu - %lu = %lu (%lu in %lums)", data, rx_bytes_last, traffic_down, data-rx_bytes_last, millis()-rx_bytes_when);
        }
        rx_bytes_last = data;
        rx_bytes_when = millis();
      }
      break;

    case STATE_IFTABLE:
      if(strncmp(ifdescr_oid, oid, strlen(ifdescr_oid))==0) {
        pdu.value.decode(str, sizeof(str) - 1);
        debug("  found %s at ifnum %d", str, ifnum);
        if(strcmp(str, ifname)==0) {
          return true;
        } else {
          return false;
        }
      }
      break;

    default:
      debug("  no idea what to do with %s", oid);
      return false;
  }
  

  last_comm = millis();

  debug("  returning true");
  return true;
}


bool process_webserver_command(WiFiClient client, char *cmd, char *uri) {
  const char *response_200 = "HTTP/1.1 200 OK";
  const char *response_404 = "HTTP/1.1 404 Not found";
  const char *response_501 = "HTTP/1.1 501 Not implemented";
  const char *response_common = "Content-Type: text/html\r\n";
  bool result = false, header_only = false;

  if(strcmp(cmd, "HEAD") == 0) {
    header_only = true;
  } else if(strcmp(cmd, "GET") != 0) {
    client.println(response_501);
    client.println(response_common);
    client.println("<h1>Not implemented</h1>");
  }

  if(strcmp(uri, uri_index) == 0) {
    client.println(response_200);
    client.println(response_common);
    client.println("<h1>piss</h1>");
    result = true;
  } else if(strcmp(uri, uri_mode) == 0) {
    client.println(response_200);
    client.println(response_common);
    client.println("<h1>New mode selected</h1>");
    new_mode();
    result = true;
  } else {
    client.println(response_404);
    client.println(response_common);
    client.println("<h1>Not found</h1>");
  }

  delay(HTTP_END_DELAY);
  return result;
}


bool process_webserver_client(void) {
  char buf[1024], *c=buf, *cmd, *uri;
  const char *space=" ";
  bool result = false;
  
  WiFiClient client = server.available();
  if(!client) {
    return result;
  }
  
  client.remoteIP().toString().toCharArray(buf, 1024);
  debug("process_webserver_client: new connection from %s", buf);

  long start = millis();

  for(;;) {
    if(client.available()) {
      *c = client.read();
      if(*c == '\r' || *c == '\n') {
        *++c = '\0';
        debug("got command: %s", buf);
        cmd = strtok(buf, space);
        if(cmd) {
          uri = strtok(NULL, space);
          if(uri) {
            result = process_webserver_command(client, cmd, uri);
          } else {
            debug("process_webserver_client: no cmd found in request");
          }
        } else {
          debug("process_webserver_client: no uri found in request");
        }
        break;
      }

      c++;
    }
    if(millis() - start >= HTTP_TIMEOUT) {
      debug("process_webserver_client: timeout while reading command");
      break;
    }
  }

  client.stop();
  return result;
}


void loop() {
  static unsigned long last_poll, mode_button_down;
  static bool give_me_a_ping;
  static int get_stats_countdown = 5;

  switch(current_state) {
    case STATE_CONNECTING:
      if(WiFi.status() == WL_CONNECTED) {
        new_state(STATE_IFTABLE);
        SNMP.begin(snmp_community, snmp_community, snmp_community, local_port);
        send_snmp_iftable();

        ntp.begin();
      }
      break;

    case STATE_IFTABLE:
      server.begin();

      if(SNMP.listen()) {
        if(recv_snmp()) {
          new_state(STATE_OK);
        } else {
          if(! send_snmp_iftable()) {
            // We couldn't find an interface with the name we want
            debug("*** %s not found!", ifname);
            new_state(STATE_NO_IF);
          }
        }
      }
      break;

    case STATE_NO_IF:
      // Hang forever, we're knackered
      break;

    default:
      if(SNMP.listen()) {
        recv_snmp();
      }
      if (millis() - last_poll > 1000) {
        debug("Polling");
        if(give_me_a_ping) {
          send_ping();
        }
  
        if(! --get_stats_countdown) {
          send_snmp(tx_oid, ifnum);
          send_snmp(rx_oid, ifnum);
          
          get_stats_countdown = 5;
        }
  
        ntp.update();

        give_me_a_ping = ! give_me_a_ping;
        last_poll = millis();
      }
  
      break;
  }

  if(current_state != STATE_NO_IF) {
    if(millis() - last_comm > GENERAL_TIMEOUT) {
      new_state(STATE_NO_COMM);
    } else if(millis() - last_ping > GENERAL_TIMEOUT) {
      new_state(STATE_NO_INTERNET);
    } else {
      if(current_state != STATE_CONNECTING && current_state != STATE_IFTABLE) {
        if(ping_time > ping_critical) {
          new_state(STATE_CRITICAL);
        } else if(ping_time > ping_warn) {
          new_state(STATE_WARNING);
        } else {
          new_state(STATE_OK);
        }
      }
    }
  }
  
  if(digitalRead(MODE_BUTTON) == LOW) {
    if(! mode_button_down) {
      mode_button_down = millis();
    }
  } else if(mode_button_down) {
    new_mode();
    mode_button_down = 0;
  }

  mode_functions[current_mode]();

  if(server.hasClient()) {
    process_webserver_client();
  }
}

