#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init(const char *ssid, const char *password);
const char* wifi_get_ip_string(void);

#ifdef __cplusplus
}
#endif