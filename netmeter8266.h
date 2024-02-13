// Network stuff
const char *wifi_network_name = "Moink";
const char *wifi_network_password = "l1tltotL";

const IPAddress router(10,20,30,254);
const IPAddress ping_target (1,1,1,1);

const char *snmp_community = "public";
const char *ntp_server  = "little.ruby.chard.org";

const char *ifname      = "eth0"; // we look for an interface with this name

const char *unifi_api   = "https://unifi-ctrl.ruby.chard.org:8443";
const char *unifi_username = "api";
const char *unifi_password = "api";