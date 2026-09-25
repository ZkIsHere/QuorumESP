/* Dev Wi-Fi STA transport. See wifi.h: NON-PRODUCTION. */
#include "wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "config.h"

static const char *TAG = "NETWORK";

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10

static EventGroupHandle_t s_ev;
static int s_retry = 0;
static uint32_t s_ip_be = 0;
static int s_rssi = 0;
/* 0 until the first GOT_IP: bounded retries, then boot fails loudly.
 * 1 afterwards: link losses retry forever (a quorum box must come back
 * on its own; sessions already fail-closed via DPD). */
static int s_boot_done = 0;

static void ev_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* data = wifi_event_sta_disconnected_t: reason tells wrong-pass
         * (AUTH_FAIL/4WAY_TIMEOUT) apart from AP-gone (NO_AP_FOUND). */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = (d != NULL) ? d->reason : 0;
        const char *hint = "";
        if (reason == WIFI_REASON_AUTH_FAIL) {
            hint = " (wrong password?)";
        } else if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                   reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            hint = " (wrong password or AP refusing?)";
        } else if (reason == WIFI_REASON_NO_AP_FOUND ||
                   reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
                   reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD) {
            hint = " (SSID not visible: wrong SSID / 5GHz-only / AP down?)";
        }
        if (s_boot_done) {
            /* Post-boot: AP reboot, interference, roaming — keep trying.
             * No counter: giving up here would leave a silent offline box
             * that only a power cycle revives. Clients see DPD timeouts
             * and reconnect when we are back (fail-closed on their side). */
            ESP_LOGW(TAG, "link lost after boot, reconnecting (retry %d, reason %u%s)",
                     s_retry + 1, reason, hint);
            s_retry++;
            s_ip_be = 0;
            esp_wifi_connect();
        } else if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGI(TAG, "wifi retry %d/%d (reason %u%s)", s_retry,
                     WIFI_MAX_RETRY, reason, hint);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_ev, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_ip_be = ev->ip_info.ip.addr;
        if (!s_boot_done) {
            s_boot_done = 1;
        } else {
            ESP_LOGW(TAG, "link recovered");
        }
        s_retry = 0;
        xEventGroupSetBits(s_ev, WIFI_GOT_IP_BIT);
    }
}

esp_err_t network_wifi_connect(uint32_t *out_ip_be) {
    EventBits_t bits;
    wifi_config_t cfg = {0};

    ESP_LOGW(TAG, "DEV TRANSPORT IS WI-FI (non-production, see docs/hardware.md)");

    if (quorumesp_config_get_wifi((char *)cfg.sta.ssid,
                                  sizeof(cfg.sta.ssid),
                                  (char *)cfg.sta.password,
                                  sizeof(cfg.sta.password)) != ESP_OK) {
        ESP_LOGE(TAG, "no Wi-Fi credentials: provision NVS (docs/provisioning.md) or set via menuconfig");
        return ESP_FAIL;
    }

    s_ev = xEventGroupCreate();
    if (s_ev == NULL) {
        return ESP_FAIL;
    }
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) {
        return ESP_FAIL;
    }
    {
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        const qesp_config_t *ccfg = quorumesp_config_get();
        if (sta == NULL) {
            return ESP_FAIL;
        }
        if (ccfg != NULL) {
            if (ccfg->hostname[0] != '\0') {
                esp_netif_set_hostname(sta, ccfg->hostname);
            }
            if (ccfg->net_static && ccfg->ip != 0 && ccfg->mask != 0) {
                esp_netif_ip_info_t info;
                info.ip.addr = ccfg->ip;
                info.netmask.addr = ccfg->mask;
                info.gw.addr = ccfg->gw;
                esp_netif_dhcpc_stop(sta);
                esp_netif_set_ip_info(sta, &info);
                if (ccfg->dns1 != 0) {
                    esp_netif_dns_info_t dns;
                    dns.ip.u_addr.ip4.addr = ccfg->dns1;
                    dns.ip.type = ESP_IPADDR_TYPE_V4;
                    esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
                }
                if (ccfg->dns2 != 0) {
                    esp_netif_dns_info_t dns;
                    dns.ip.u_addr.ip4.addr = ccfg->dns2;
                    dns.ip.type = ESP_IPADDR_TYPE_V4;
                    esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns);
                }
                ESP_LOGI(TAG, "static IP from config");
            }
        }
    }
    {
        wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&icfg) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            ev_handler, NULL, NULL) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            ev_handler, NULL, NULL) != ESP_OK) {
        return ESP_FAIL;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return ESP_FAIL;
    }
    /* Dev workaround for weak USB power (brownout on TX bursts):
     * cap TX at ~13 dBm instead of 20 dBm. Production hardware with a
     * proper supply + Ethernet does not need this. Unit: 0.25 dBm. */
    if (esp_wifi_set_max_tx_power(52) != ESP_OK) {
        ESP_LOGW(TAG, "could not cap tx power");
    }

    bits = xEventGroupWaitBits(s_ev, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                               pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & WIFI_GOT_IP_BIT) == 0) {
        ESP_LOGE(TAG, "wifi failed after %d retries", WIFI_MAX_RETRY);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wifi up, ip=" IPSTR, IP2STR((esp_ip4_addr_t *)&s_ip_be));
    if (out_ip_be != NULL) {
        *out_ip_be = s_ip_be;
    }
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
    }
    return ESP_OK;
}

uint32_t network_wifi_get_ip(void) {
    return s_ip_be;
}

int network_wifi_get_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    return s_rssi;
}
