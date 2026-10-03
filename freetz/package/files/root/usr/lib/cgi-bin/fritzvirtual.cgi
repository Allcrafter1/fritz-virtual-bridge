#!/bin/sh
# SPDX-License-Identifier: MIT

. /usr/lib/libmodcgi.sh

sec_begin "$(lang de:"Dienst" en:"Service")"
cgi_print_radiogroup_service_starttype "enabled" "$FRITZVIRTUAL_ENABLED" "" "" 0
sec_end

sec_begin "MQTT"
cgi_print_textline_p "mqtt_host" "$FRITZVIRTUAL_MQTT_HOST" 40/255 "$(lang de:"Broker-Host" en:"Broker host"): "
cgi_print_textline_p "mqtt_port" "$FRITZVIRTUAL_MQTT_PORT" 6/5 "Port: "
cgi_print_textline_p "mqtt_username" "$FRITZVIRTUAL_MQTT_USERNAME" 40/255 "$(lang de:"Benutzername" en:"Username"): "
cgi_print_password_p "mqtt_password" "$FRITZVIRTUAL_MQTT_PASSWORD" 40/255 "$(lang de:"Passwort" en:"Password"): "
cgi_print_textline_p "bridge_id" "$FRITZVIRTUAL_BRIDGE_ID" 32/32 "Bridge ID: "
cgi_print_checkbox "debug" "$FRITZVIRTUAL_DEBUG" "$(lang de:"Debug-Protokoll" en:"Debug logging")"
sec_end

sec_begin "$(lang de:"Hinweis" en:"Note")"
cat <<'EOF'
<p>FRITZ!OS bleibt der Layout-Editor f&uuml;r das FRITZ!Smart Control 440.
Virtuelle Ger&auml;te werden in Home Assistant angelegt und danach in FRITZ!OS
einem Anzeigeplatz zugewiesen.</p>
EOF
sec_end
