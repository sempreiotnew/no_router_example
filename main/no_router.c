/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fire Alarm — single file with retry + tree topology log.
 *
 * ROOT  : broadcasts DATA, waits ACK_WINDOW_MS, retries up to
 *         MAX_RETRIES times for nodes that have not ACKed.
 *         Only advances seq once ALL nodes have ACKed (or retry
 *         limit is reached and the miss is logged).
 *
 * NODE  : receives DATA, blinks, forwards, sends ACK upstream.
 *         Also forwards child ACKs upstream.
 *
 * Topology log: printed as an ASCII tree showing each level.
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <inttypes.h>
#include <string.h>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <arpa/inet.h>
#include <sys/socket.h>

#include "driver/gpio.h"
#include "esp_bridge.h"
#include "esp_mac.h"
#include "esp_mesh_lite.h"

/* ─── Ports ───────────────────────────────────────────────────────── */
#define ALARM_DATA_PORT 3333
#define ALARM_ACK_PORT 3334

/* ─── Protocol ────────────────────────────────────────────────────── */
#define PFX_DATA "D:"
#define PFX_ACK "A:"
#define ALARM_PAYLOAD "ALARM:HIGH"
#define DATA_FMT "D:ALARM:%s:seq=%" PRIu32 ":mac=%02x:%02x:%02x:%02x:%02x:%02x"
#define ACK_FMT "A:seq=%" PRIu32 ":mac=%02x:%02x:%02x:%02x:%02x:%02x"

/* ─── Tuning ──────────────────────────────────────────────────────── */
#define MSG_MAX_LEN 128
#define RX_QUEUE_DEPTH 32
#define TX_QUEUE_DEPTH 8
#define ACK_QUEUE_DEPTH 64
#define LED_GPIO 2

/* Retry config */
#define ACK_WINDOW_MS 1500    /* wait this long before checking ACKs  */
#define MAX_RETRIES 10        /* retransmit up to N times per seq      */
#define RETRY_DELAY_MS 800    /* wait between retries                  */
#define SEND_INTERVAL_MS 3000 /* gap between new seq (after all ACKs)  */

#define MAX_TRACKED_NODES 32

#define BUTTON_GPIO GPIO_NUM_0

static const char *TAG = "fire_alarm";

bool button_on_off = false;
bool is_alarm = false;
volatile bool led_state = false;
static bool s_wifi_connected = false;

uint8_t root_mac[6];

/* ─── Message envelope ────────────────────────────────────────────── */
typedef struct {
  char data[MSG_MAX_LEN];
  uint16_t len;
} mesh_msg_t;

/* ─── Queues ──────────────────────────────────────────────────────── */
static QueueHandle_t s_tx_queue = NULL;
static QueueHandle_t s_ack_queue = NULL;
#if !CONFIG_MESH_ROOT
static QueueHandle_t s_rx_queue = NULL;
static QueueHandle_t s_fwd_ack_queue = NULL;
#endif

/* ─── Stats ───────────────────────────────────────────────────────── */
static uint32_t s_tx_ok = 0;
static uint32_t s_tx_fail = 0;
static uint32_t s_retries = 0;
#if !CONFIG_MESH_ROOT
static uint32_t s_rx_count = 0;
static uint32_t s_rx_overflow = 0;
static uint32_t s_fwd_ok = 0;
static uint32_t s_fwd_fail = 0;
static uint32_t s_ack_sent = 0;
#endif

/* ═══════════════════════════════════════════════════════════════════
 * ROOT — delivery tracking
 * ═══════════════════════════════════════════════════════════════════ */
#if CONFIG_MESH_ROOT

typedef struct {
  uint8_t mac[6];
  int level; /* populated from mesh node list if available */
  bool valid;
} tracked_node_t;

typedef struct {
  uint32_t seq;
  bool acked[MAX_TRACKED_NODES];
  bool active;
} pending_t;

static tracked_node_t s_nodes[MAX_TRACKED_NODES];
static int s_node_count = 0;
static pending_t s_pending = {0};

static SemaphoreHandle_t s_nodes_mutex = NULL;

static int track_node_locked(const uint8_t mac[6], int level) {
  for (int i = 0; i < s_node_count; i++)
    if (memcmp(s_nodes[i].mac, mac, 6) == 0) {
      if (level > 0)
        s_nodes[i].level = level;
      return i;
    }
  if (s_node_count >= MAX_TRACKED_NODES)
    return -1;
  memcpy(s_nodes[s_node_count].mac, mac, 6);
  s_nodes[s_node_count].level = level;
  s_nodes[s_node_count].valid = true;
  ESP_LOGI(TAG, "[ROOT] new node [%d] level=%d mac:" MACSTR, s_node_count,
           level, MAC2STR(mac));
  return s_node_count++;
}

static void record_ack(uint32_t seq, const uint8_t mac[6]) {
  if (!s_pending.active || s_pending.seq != seq)
    return;
  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
  int idx = track_node_locked(mac, 0);
  if (idx >= 0)
    s_pending.acked[idx] = true;
  xSemaphoreGive(s_nodes_mutex);
}

/* Returns true if all known nodes have ACKed */
static bool all_acked(void) {
  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
  bool ok = true;
  for (int i = 0; i < s_node_count; i++)
    if (!s_pending.acked[i]) {
      ok = false;
      break;
    }
  xSemaphoreGive(s_nodes_mutex);
  return (s_node_count > 0) && ok;
}

static int count_acked(void) {
  int c = 0;
  for (int i = 0; i < s_node_count; i++)
    if (s_pending.acked[i])
      c++;
  return c;
}

/* ── ASCII tree topology print ──────────────────────────────────────
 * Groups nodes by level and prints as:
 *
 *  MESH TOPOLOGY (4 nodes)
 *  └── [ROOT] d4:e9:f4:bc:a2:c8  (level 1)
 *      ├── [L2] 40:22:d8:7b:84:80  ✓ acked
 *      ├── [L2] 00:4b:12:2d:f7:8c  ✓ acked
 *      │   ├── [L3] e0:5a:1b:50:22:68  ✗ MISSING
 *      │   └── [L3] 38:18:2b:8c:13:d0  ✗ MISSING
 */
// static void print_tree(uint32_t seq) {
//   /* Gather levels — use mesh node list to enrich level info */
//   uint32_t mesh_size = 0;
//   const node_info_list_t *node = esp_mesh_lite_get_nodes_list(&mesh_size);
//   xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
//   for (uint32_t i = 0; i < mesh_size && node; i++) {
//     // track_node_locked(node->node->mac_addr, node->node->level);
//     if (memcmp(node->node->mac_addr, root_mac, 6) != 0) {
//       track_node_locked(node->node->mac_addr, node->node->level);
//     }
//     node = node->next;
//   }

//   uint8_t root_mac[6];
//   esp_wifi_get_mac(WIFI_IF_STA, root_mac);

//   ESP_LOGI(TAG, "");
//   ESP_LOGI(TAG, "  MESH TOPOLOGY  seq=%" PRIu32 "  (%d tracked nodes)", seq,
//            s_node_count);
//   ESP_LOGI(TAG, "  └── [ROOT] " MACSTR, MAC2STR(root_mac));

//   /* Print each level from 2 upward */
//   int max_level = 2;
//   for (int i = 0; i < s_node_count; i++)
//     if (s_nodes[i].level > max_level)
//       max_level = s_nodes[i].level;

//   for (int lvl = 2; lvl <= max_level; lvl++) {
//     /* count nodes at this level for tree branch drawing */
//     int cnt = 0;
//     for (int i = 0; i < s_node_count; i++)
//       if (s_nodes[i].level == lvl)
//         cnt++;
//     int drawn = 0;
//     for (int i = 0; i < s_node_count; i++) {
//       if (s_nodes[i].level != lvl)
//         continue;
//       drawn++;
//       const char *branch = (drawn < cnt) ? "├──" : "└──";
//       /* build indent based on level */
//       char indent[64] = "  ";
//       for (int d = 2; d < lvl; d++)
//         strncat(indent, "│   ", sizeof(indent) - strlen(indent) - 1);

//       if (s_pending.active && s_pending.seq == seq) {
//         bool acked = s_pending.acked[i];
//         if (acked) {
//           ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR "  \u2713 acked", indent,
//           branch,
//                    lvl, MAC2STR(s_nodes[i].mac));
//         } else {
//           ESP_LOGW(TAG, "  %s%s [L%d] " MACSTR "  \u2717 MISSING", indent,
//                    branch, lvl, MAC2STR(s_nodes[i].mac));
//         }
//       } else {
//         ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR, indent, branch, lvl,
//                  MAC2STR(s_nodes[i].mac));
//       }
//     }
//   }
//   ESP_LOGI(TAG, "");
//   xSemaphoreGive(s_nodes_mutex);
// }

// static void print_tree(uint32_t seq) {
//   uint8_t root_mac[6];
//   esp_wifi_get_mac(WIFI_IF_STA, root_mac);

//   /* OPTIONAL: try to enrich levels, but DO NOT override tracked nodes */
//   uint32_t mesh_size = 0;
//   const node_info_list_t *node = esp_mesh_lite_get_nodes_list(&mesh_size);

//   xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);

//   while (node) {
//     for (int i = 0; i < s_node_count; i++) {
//       if (memcmp(s_nodes[i].mac, node->node->mac_addr, 6) == 0) {
//         // update level only if found
//         s_nodes[i].level = node->node->level;
//         break;
//       }
//     }
//     node = node->next;
//   }

//   ESP_LOGI(TAG, "");
//   ESP_LOGI(TAG, "  MESH TOPOLOGY  seq=%" PRIu32 "  (%d tracked nodes)", seq,
//            s_node_count);
//   ESP_LOGI(TAG, "  └── [ROOT] " MACSTR, MAC2STR(root_mac));

//   /* ensure ALL nodes appear (even without level) */
//   int max_level = 2;
//   for (int i = 0; i < s_node_count; i++) {
//     if (s_nodes[i].level == 0) {
//       s_nodes[i].level = 2; // fallback
//     }
//     if (s_nodes[i].level > max_level)
//       max_level = s_nodes[i].level;
//   }

//   for (int lvl = 2; lvl <= max_level; lvl++) {
//     int cnt = 0;
//     for (int i = 0; i < s_node_count; i++)
//       if (s_nodes[i].level == lvl)
//         cnt++;

//     int drawn = 0;
//     for (int i = 0; i < s_node_count; i++) {
//       if (s_nodes[i].level != lvl)
//         continue;

//       drawn++;
//       const char *branch = (drawn < cnt) ? "├──" : "└──";

//       char indent[64] = "  ";
//       for (int d = 2; d < lvl; d++)
//         strncat(indent, "│   ", sizeof(indent) - strlen(indent) - 1);

//       if (s_pending.active && s_pending.seq == seq) {
//         bool acked = s_pending.acked[i];
//         if (acked) {
//           ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR "  \u2713 acked", indent,
//           branch,
//                    lvl, MAC2STR(s_nodes[i].mac));
//         } else {
//           ESP_LOGW(TAG, "  %s%s [L%d] " MACSTR "  \u2717 MISSING", indent,
//                    branch, lvl, MAC2STR(s_nodes[i].mac));
//         }
//       } else {
//         ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR, indent, branch, lvl,
//                  MAC2STR(s_nodes[i].mac));
//       }
//     }
//   }

//   ESP_LOGI(TAG, "");
//   xSemaphoreGive(s_nodes_mutex);
// }
static void print_tree(uint32_t seq) {
  uint8_t root_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, root_mac);

  /* OPTIONAL: enrich levels WITHOUT overriding */
  uint32_t mesh_size = 0;
  const node_info_list_t *node = esp_mesh_lite_get_nodes_list(&mesh_size);

  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);

  while (node) {
    for (int i = 0; i < s_node_count; i++) {
      if (memcmp(s_nodes[i].mac, node->node->mac_addr, 6) == 0) {
        // ONLY update if mesh gives a valid level (>0)
        if (node->node->level > 0) {
          s_nodes[i].level = node->node->level;
        }
        break;
      }
    }
    node = node->next;
  }

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "Internet : %s", s_wifi_connected ? "connected" : "offline");
  ESP_LOGI(TAG, "  MESH TOPOLOGY  seq=%" PRIu32 "  (%d tracked nodes)", seq,
           s_node_count);
  ESP_LOGI(TAG, "  └── [ROOT] " MACSTR, MAC2STR(root_mac));

  /* find max level WITHOUT modifying data */
  int max_level = 1;
  for (int i = 0; i < s_node_count; i++) {
    if (s_nodes[i].level > max_level)
      max_level = s_nodes[i].level;
  }

  /* print ALL nodes — even if level is unknown (0) */
  for (int lvl = 1; lvl <= max_level; lvl++) {
    int cnt = 0;

    for (int i = 0; i < s_node_count; i++) {
      if (s_nodes[i].level == lvl)
        cnt++;
    }

    int drawn = 0;

    for (int i = 0; i < s_node_count; i++) {
      if (s_nodes[i].level != lvl)
        continue;

      drawn++;
      const char *branch = (drawn < cnt) ? "├──" : "└──";

      char indent[64] = "  ";
      for (int d = 1; d < lvl; d++) {
        strncat(indent, "│   ", sizeof(indent) - strlen(indent) - 1);
      }

      if (s_pending.active && s_pending.seq == seq) {
        bool acked = s_pending.acked[i];

        if (acked) {
          ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR "  \u2713 acked", indent, branch,
                   lvl, MAC2STR(s_nodes[i].mac));
        } else {
          ESP_LOGW(TAG, "  %s%s [L%d] " MACSTR "  \u2717 MISSING", indent,
                   branch, lvl, MAC2STR(s_nodes[i].mac));
        }
      } else {
        ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR, indent, branch, lvl,
                 MAC2STR(s_nodes[i].mac));
      }
    }
  }

  /* 🔥 EXTRA: print nodes with unknown level (level == 0) */
  int unknown_cnt = 0;
  for (int i = 0; i < s_node_count; i++) {
    if (s_nodes[i].level == 0)
      unknown_cnt++;
  }

  if (unknown_cnt > 0) {
    int drawn = 0;
    for (int i = 0; i < s_node_count; i++) {
      if (s_nodes[i].level != 0)
        continue;

      drawn++;
      const char *branch = (drawn < unknown_cnt) ? "├──" : "└──";

      if (s_pending.active && s_pending.seq == seq) {
        bool acked = s_pending.acked[i];

        if (acked) {
          ESP_LOGI(TAG, "    %s [L?] " MACSTR "  %s", branch,
                   MAC2STR(s_nodes[i].mac), "✓ acked");
        } else {
          ESP_LOGW(TAG, "    %s [L?] " MACSTR "  %s", branch,
                   MAC2STR(s_nodes[i].mac), "✗ MISSING");
        }

      } else {
        ESP_LOGI(TAG, "    %s [L?] " MACSTR, branch, MAC2STR(s_nodes[i].mac));
      }
    }
  }

  ESP_LOGI(TAG, "");
  xSemaphoreGive(s_nodes_mutex);
}

#endif /* CONFIG_MESH_ROOT */

/* ═══════════════════════════════════════════════════════════════════
 * LED — non-blocking blink via task notification
 * ═══════════════════════════════════════════════════════════════════ */
static TaskHandle_t s_blink_handle = NULL;

static void blink_task(void *arg) {
  gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_GPIO, 0);
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // gpio_set_level(LED_GPIO, 1);
    // vTaskDelay(pdMS_TO_TICKS(40));
    // gpio_set_level(LED_GPIO, 0);
    // vTaskDelay(pdMS_TO_TICKS(40));
    uint32_t level = led_state ? 1 : 0;
    gpio_set_level(LED_GPIO, level);
  }
}

static inline void blink_led(void) {
  if (s_blink_handle)
    xTaskNotifyGive(s_blink_handle);
}

/* ═══════════════════════════════════════════════════════════════════
 * Network helpers
 * ═══════════════════════════════════════════════════════════════════ */
static bool get_parent_ip(esp_ip4_addr_t *out) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta)
    return false;
  esp_netif_ip_info_t info = {0};
  esp_netif_get_ip_info(sta, &info);
  if (info.gw.addr == 0)
    return false;
  *out = info.gw;
  return true;
}

static uint32_t get_ap_broadcast(void) {
  esp_netif_ip_info_t info = {0};
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap)
    esp_netif_get_ip_info(ap, &info);
  return (info.ip.addr & info.netmask.addr) | ~info.netmask.addr;
}

static esp_err_t udp_broadcast(const char *msg, uint16_t len) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
    return ESP_FAIL;
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  struct sockaddr_in dest = {
      .sin_family = AF_INET,
      .sin_port = htons(ALARM_DATA_PORT),
      .sin_addr.s_addr = get_ap_broadcast(),
  };
  int r = sendto(sock, msg, len, 0, (struct sockaddr *)&dest, sizeof(dest));
  close(sock);
  return (r > 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t udp_unicast_parent(const char *msg, uint16_t len,
                                    uint16_t port) {
  esp_ip4_addr_t gw;
  if (!get_parent_ip(&gw))
    return ESP_ERR_NOT_FOUND;
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
    return ESP_FAIL;
  struct sockaddr_in dest = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = gw.addr,
  };
  int r = sendto(sock, msg, len, 0, (struct sockaddr *)&dest, sizeof(dest));
  close(sock);
  return (r > 0) ? ESP_OK : ESP_FAIL;
}

/* ═══════════════════════════════════════════════════════════════════
 * ROOT — produce + retry loop
 *
 * Flow per seq:
 *   1. broadcast DATA
 *   2. wait ACK_WINDOW_MS
 *   3. if all acked → next seq
 *   4. else retry up to MAX_RETRIES times for missing nodes
 *   5. report final delivery status with tree
 * ═══════════════════════════════════════════════════════════════════ */
#if CONFIG_MESH_ROOT

static void root_send_task(void *arg) {
  mesh_msg_t msg;
  while (1) {
    if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE)
      continue;
    esp_err_t err = udp_broadcast(msg.data, msg.len);
    if (err == ESP_OK) {
      s_tx_ok++;
      ESP_LOGI(TAG, "[ROOT-SEND] TX  '%s'  (total_sent=%" PRIu32 ")", msg.data,
               s_tx_ok);
    } else {
      s_tx_fail++;
      ESP_LOGW(TAG, "[ROOT-SEND] TX FAIL  (fail=%" PRIu32 ")", s_tx_fail);
    }
  }
}

/* ROOT — ACK collector */
static void root_ack_rx_task(void *arg) {
  int sock = -1;
  while (sock < 0) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(ALARM_ACK_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[ROOT-ACK] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[ROOT-ACK] listening on port %d", ALARM_ACK_PORT);

  mesh_msg_t msg;
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);

  while (1) {
    int len = recvfrom(sock, msg.data, MSG_MAX_LEN - 1, 0,
                       (struct sockaddr *)&src, &src_len);
    if (len <= 0)
      continue;
    msg.data[len] = '\0';
    msg.len = (uint16_t)len;
    xQueueSend(s_ack_queue, &msg, 0);
  }
}

/* ROOT — ACK processor */
static void root_ack_process_task(void *arg) {
  mesh_msg_t msg;
  while (1) {
    if (xQueueReceive(s_ack_queue, &msg, portMAX_DELAY) != pdTRUE)
      continue;
    if (strncmp(msg.data, PFX_ACK, strlen(PFX_ACK)) != 0)
      continue;

    uint32_t seq = 0;
    unsigned int m[6] = {0};
    if (sscanf(msg.data, "A:seq=%" PRIu32 ":mac=%x:%x:%x:%x:%x:%x", &seq, &m[0],
               &m[1], &m[2], &m[3], &m[4], &m[5]) == 7) {
      uint8_t mac[6];
      for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)m[i];
      record_ack(seq, mac);

      int acked = count_acked();
      int missing = s_node_count - acked;
      ESP_LOGI(TAG,
               "[ROOT-ACK] seq=%" PRIu32 "  from " MACSTR
               "  acked=%d/%d  missing=%d",
               seq, MAC2STR(mac), acked, s_node_count, missing);
    }
  }
}

#endif /* CONFIG_MESH_ROOT */

/* ═══════════════════════════════════════════════════════════════════
 * NODE — RX task (prio 7, never stalls)
 * ═══════════════════════════════════════════════════════════════════ */
#if !CONFIG_MESH_ROOT
static void node_rx_task(void *arg) {
  int sock = -1;
  while (sock < 0) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(ALARM_DATA_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[NODE-RX] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[NODE-RX] ready  level=%d", esp_mesh_lite_get_level());

  mesh_msg_t msg;
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);

  while (1) {
    int len = recvfrom(sock, msg.data, MSG_MAX_LEN - 1, 0,
                       (struct sockaddr *)&src, &src_len);
    if (len <= 0) {
      //   ESP_LOGI(TAG, "[NODE-RX] idle  rx=%" PRIu32 "  overflow=%" PRIu32,
      //            s_rx_count, s_rx_overflow);
      continue;
    }
    msg.data[len] = '\0';
    msg.len = (uint16_t)len;
    s_rx_count++;

    char src_ip[16];
    inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    ESP_LOGI(TAG, "[NODE-RX] from=%s  '%s'  (rx=%" PRIu32 ")", src_ip, msg.data,
             s_rx_count);

    if (xQueueSend(s_rx_queue, &msg, 0) != pdTRUE) {
      s_rx_overflow++;
      ESP_LOGW(TAG, "[NODE-RX] QUEUE FULL  overflow=%" PRIu32, s_rx_overflow);
    }
  }
}

/* NODE — ACK RX: receives child ACKs, enqueues for forwarding upstream */
static void node_ack_rx_task(void *arg) {
  int sock = -1;
  while (sock < 0) {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(ALARM_ACK_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[NODE-ACK-RX] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[NODE-ACK-RX] ready");

  mesh_msg_t msg;
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);
  while (1) {
    int len = recvfrom(sock, msg.data, MSG_MAX_LEN - 1, 0,
                       (struct sockaddr *)&src, &src_len);
    if (len <= 0)
      continue;
    msg.data[len] = '\0';
    msg.len = (uint16_t)len;
    char src_ip[16];
    inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    ESP_LOGI(TAG, "[NODE-ACK-RX] child ACK from=%s  '%s'", src_ip, msg.data);
    xQueueSend(s_fwd_ack_queue, &msg, 0);
  }
}

/* NODE — forward + ACK task */
static void node_forward_task(void *arg) {
  mesh_msg_t msg;
  uint32_t last_seq = UINT32_MAX;

  while (1) {
    if (xQueueReceive(s_rx_queue, &msg, portMAX_DELAY) != pdTRUE)
      continue;

    // if (strncmp(msg.data, PFX_DATA, strlen(PFX_DATA)) == 0)
    //   blink_led();

    uint32_t seq = 0;
    unsigned int m[6] = {0};
    // bool parsed =
    //     (sscanf(msg.data, "D:ALARM:HIGH:seq=%" PRIu32
    //     ":mac=%x:%x:%x:%x:%x:%x",
    //             &seq, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 7);
    char alarm_state[6] = {0}; // "true" or "false"

    bool parsed =
        (sscanf(
             msg.data, "D:ALARM:%5[^:]:seq=%" PRIu32 ":mac=%x:%x:%x:%x:%x:%x",
             alarm_state, &seq, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 8);
    bool is_alarm = (strcmp(alarm_state, "true") == 0);

    if (parsed) {
      led_state = is_alarm;
      xTaskNotifyGive(s_blink_handle); // reuse handle
    }

    /* Gap detection */
    if (parsed && last_seq != UINT32_MAX && seq != last_seq + 1)
      ESP_LOGW(TAG,
               "[NODE-FWD] GAP  expected=%" PRIu32 " got=%" PRIu32
               " missed=%" PRIu32,
               last_seq + 1, seq, seq - last_seq - 1);
    if (parsed)
      last_seq = seq;

    /* Forward downstream */
    wifi_sta_list_t sl = {0};
    esp_wifi_ap_get_sta_list(&sl);
    if (sl.num > 0) {
      if (udp_broadcast(msg.data, msg.len) == ESP_OK) {
        s_fwd_ok++;
        ESP_LOGI(TAG,
                 "[NODE-FWD] FWD OK  seq=%" PRIu32
                 "  children=%d  (fwd=%" PRIu32 ")",
                 seq, sl.num, s_fwd_ok);
      } else {
        s_fwd_fail++;
        ESP_LOGW(TAG,
                 "[NODE-FWD] FWD FAIL  seq=%" PRIu32 "  (fail=%" PRIu32 ")",
                 seq, s_fwd_fail);
      }
    }

    /* Send our own ACK upstream */
    if (parsed) {
      uint8_t my_mac[6];
      esp_wifi_get_mac(WIFI_IF_STA, my_mac);
      char ack[MSG_MAX_LEN];
      uint16_t ack_len = (uint16_t)snprintf(ack, sizeof(ack), ACK_FMT, seq,
                                            my_mac[0], my_mac[1], my_mac[2],
                                            my_mac[3], my_mac[4], my_mac[5]);

      /* Retry ACK send up to 3 times to handle transient failures */
      for (int t = 0; t < 3; t++) {
        if (udp_unicast_parent(ack, ack_len, ALARM_ACK_PORT) == ESP_OK) {
          s_ack_sent++;
          ESP_LOGI(TAG, "[NODE-FWD] ACK sent  '%s'  (ack_sent=%" PRIu32 ")",
                   ack, s_ack_sent);
          break;
        }
        ESP_LOGW(TAG, "[NODE-FWD] ACK retry %d/3", t + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }
}

/* NODE — ACK forward task: relays child ACKs upstream */
static void node_ack_fwd_task(void *arg) {
  mesh_msg_t msg;
  while (1) {
    if (xQueueReceive(s_fwd_ack_queue, &msg, portMAX_DELAY) != pdTRUE)
      continue;
    for (int t = 0; t < 3; t++) {
      if (udp_unicast_parent(msg.data, msg.len, ALARM_ACK_PORT) == ESP_OK) {
        ESP_LOGI(TAG, "[NODE-ACK-FWD] forwarded  '%s'", msg.data);
        break;
      }
      ESP_LOGW(TAG, "[NODE-ACK-FWD] retry %d/3", t + 1);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}
#endif /* !CONFIG_MESH_ROOT */

/* ═══════════════════════════════════════════════════════════════════
 * System info timer
 * ═══════════════════════════════════════════════════════════════════ */
static void print_system_info_timercb(TimerHandle_t timer) {
  uint8_t primary = 0, sta_mac[6] = {0};
  wifi_ap_record_t ap_info = {0};
  wifi_second_chan_t second = 0;
  if (esp_mesh_lite_get_level() > 1)
    esp_wifi_sta_get_ap_info(&ap_info);
  esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
  esp_wifi_get_channel(&primary, &second);

  ESP_LOGI(TAG, "=== SYS INFO  ch:%d  level:%d  mac:" MACSTR "  heap:%" PRIu32,
           primary, esp_mesh_lite_get_level(), MAC2STR(sta_mac),
           esp_get_free_heap_size());

#if CONFIG_MESH_ROOT
  ESP_LOGI(TAG,
           "  tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "  retries=%" PRIu32
           "  tracked_nodes=%d",
           s_tx_ok, s_tx_fail, s_retries, s_node_count);
  print_tree(s_pending.seq);
#else
  esp_ip4_addr_t gw = {0};
  get_parent_ip(&gw);
  ESP_LOGI(TAG, "  parent_gw:" IPSTR "  parent_bssid:" MACSTR "  rssi:%d",
           IP2STR(&gw), MAC2STR(ap_info.bssid),
           ap_info.rssi != 0 ? ap_info.rssi : -120);
  wifi_sta_list_t sl = {0};
  esp_wifi_ap_get_sta_list(&sl);
  for (int i = 0; i < sl.num; i++)
    ESP_LOGI(TAG, "  child[%d]: " MACSTR, i, MAC2STR(sl.sta[i].mac));
  ESP_LOGI(TAG,
           "  rx=%" PRIu32 "  overflow=%" PRIu32 "  fwd_ok=%" PRIu32
           "  fwd_fail=%" PRIu32 "  ack_sent=%" PRIu32,
           s_rx_count, s_rx_overflow, s_fwd_ok, s_fwd_fail, s_ack_sent);
#endif
}

/* ─── Storage / Wi-Fi / SoftAP ────────────────────────────────────── */
static esp_err_t esp_storage_init(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  return ret;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {

  if (event_base == WIFI_EVENT) {

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
      ESP_LOGI(TAG, "✅ Connected to AP");
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
      ESP_LOGW(TAG, "❌ Disconnected from AP");
      s_wifi_connected = false;
    }
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "🔥 GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));

    s_wifi_connected = true;
  }
}

static void wifi_init(void) {
  wifi_config_t wifi_config;
  memset(&wifi_config, 0x0, sizeof(wifi_config_t));
  esp_bridge_wifi_set_config(WIFI_IF_STA, &wifi_config);

  wifi_config_t wifi_softap_config = {
      .ap =
          {
              .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
              .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
              .channel = CONFIG_MESH_CHANNEL,
          },
  };
  esp_bridge_wifi_set_config(WIFI_IF_AP, &wifi_softap_config);

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
}

#if CONFIG_MESH_ROOT
void send_broadcast() {
  uint32_t seq = 0;
  uint8_t root_mac[6];

  //   ESP_LOGI(TAG, "[ROOT-PROD] waiting 5s for mesh to form...");
  //   vTaskDelay(pdMS_TO_TICKS(5000));

  esp_wifi_get_mac(WIFI_IF_STA, root_mac);

  /* ── Arm delivery tracking ── */
  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
  s_pending.seq = seq;
  s_pending.active = true;
  memset(s_pending.acked, 0, sizeof(s_pending.acked));
  xSemaphoreGive(s_nodes_mutex);

  /* ── First transmission ── */
  mesh_msg_t msg;
  //   msg.len = (uint16_t)snprintf(msg.data, MSG_MAX_LEN, DATA_FMT, seq,
  //                                root_mac[0], root_mac[1], root_mac[2],
  //                                root_mac[3], root_mac[4], root_mac[5]);

  const char *state = button_on_off ? "true" : "false";

  msg.len =
      snprintf(msg.data, MSG_MAX_LEN, DATA_FMT, state, seq, root_mac[0],
               root_mac[1], root_mac[2], root_mac[3], root_mac[4], root_mac[5]);

  ESP_LOGI(TAG, "[ROOT-PROD] ── seq=%" PRIu32 "  nodes=%d ──", seq,
           s_node_count);
  xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200));

  /* ── Retry loop ── */
  bool delivered = false;
  for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {

    vTaskDelay(pdMS_TO_TICKS(ACK_WINDOW_MS));

    int acked = count_acked();
    ESP_LOGI(TAG, "[ROOT-PROD] attempt=%d  acked=%d/%d", attempt + 1, acked,
             s_node_count);

    if (all_acked()) {
      ESP_LOGI(TAG, "[ROOT-PROD] ALL DELIVERED  seq=%" PRIu32, seq);
      delivered = true;
      break;
    }

    // if (attempt < MAX_RETRIES) {
    //   /* Retransmit for missing nodes */
    //   s_retries++;
    //   ESP_LOGW(TAG,
    //            "[ROOT-PROD] RETRY %d/%d  seq=%" PRIu32
    //            "  missing=%d  (total_retries=%" PRIu32 ")",
    //            attempt + 1, MAX_RETRIES, seq, s_node_count - acked,
    //            s_retries);
    //   xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200));
    //   vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    // }

    if (attempt < MAX_RETRIES) {
      /* Retransmit for missing nodes */
      s_retries++;

      ESP_LOGW(TAG,
               "[ROOT-PROD] RETRY %d/%d  seq=%" PRIu32
               "  missing=%d  (total_retries=%" PRIu32 ")",
               attempt + 1, MAX_RETRIES, seq, s_node_count - acked, s_retries);

      // 🔥 ADD THIS BLOCK
      xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);

      for (int i = 0; i < s_node_count; i++) {
        if (!s_pending.acked[i]) {
          ESP_LOGW(TAG, "  → retrying node: " MACSTR, MAC2STR(s_nodes[i].mac));
        }
      }

      xSemaphoreGive(s_nodes_mutex);
      // 🔥 END

      xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(200));
      vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
    }
  }

  if (!delivered) {
    ESP_LOGE(TAG,
             "[ROOT-PROD] DELIVERY INCOMPLETE  seq=%" PRIu32
             "  acked=%d/%d after %d retries",
             seq, count_acked(), s_node_count, MAX_RETRIES);
  }

  /* ── Print tree with ACK status ── */
  print_tree(seq);

  s_pending.active = false;
  seq++;

  // vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
}

void connect_to_router(void) {
  wifi_config_t wifi_config = {0};

  strcpy((char *)wifi_config.sta.ssid, "brasil");
  strcpy((char *)wifi_config.sta.password, "brasil5g");

  ESP_LOGI(TAG, "Connecting to router SSID: brasil");

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // keep mesh + STA
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_connect());
}

static void button_pressed(void *arg) {
  int last = 1;

  while (1) {
    int cur = gpio_get_level(BUTTON_GPIO);

    if (last == 1 && cur == 0) { // button pressed
      connect_to_router();
    }

    last = cur;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void button_task(void *arg) {
  while (1) {
    // toggle state every cycle
    button_on_off = !button_on_off;

    send_broadcast(); // runs fully (blocking until done)

    vTaskDelay(pdMS_TO_TICKS(10000)); // wait before next cycle
  }
}

void gpio_init_all() {
  gpio_config_t led = {.pin_bit_mask = 1ULL << LED_GPIO,
                       .mode = GPIO_MODE_OUTPUT};
  gpio_config(&led);

  gpio_config_t btn = {.pin_bit_mask = 1ULL << BUTTON_GPIO,
                       .mode = GPIO_MODE_INPUT,
                       .pull_up_en = GPIO_PULLUP_ENABLE};
  gpio_config(&btn);
  xTaskCreate(button_task, "button", 8096, NULL, 4, NULL);
}

// void send_task() {
//   button_on_off = !button_on_off;
//   send_broadcast();
// }
#endif CONFIG_MESH_ROOT

void app_wifi_set_softap_info(void) {
  char softap_ssid[33];
  char softap_psw[64];
  uint8_t softap_mac[6];
  size_t ssid_size = sizeof(softap_ssid);
  size_t psw_size = sizeof(softap_psw);

  esp_wifi_get_mac(WIFI_IF_AP, softap_mac);
  memset(softap_ssid, 0x0, sizeof(softap_ssid));
  memset(softap_psw, 0x0, sizeof(softap_psw));

  if (esp_mesh_lite_get_softap_ssid_from_nvs(softap_ssid, &ssid_size) ==
      ESP_OK) {
    ESP_LOGI(TAG, "Get ssid from nvs: %s", softap_ssid);
  } else {
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf(softap_ssid, sizeof(softap_ssid), "%.25s_%02x%02x%02x",
             CONFIG_BRIDGE_SOFTAP_SSID, softap_mac[3], softap_mac[4],
             softap_mac[5]);
#else
    snprintf(softap_ssid, sizeof(softap_ssid), "%.32s",
             CONFIG_BRIDGE_SOFTAP_SSID);
#endif
    ESP_LOGI(TAG, "Get ssid from nvs failed, set ssid: %s", softap_ssid);
  }

  if (esp_mesh_lite_get_softap_psw_from_nvs(softap_psw, &psw_size) == ESP_OK) {
    ESP_LOGI(TAG, "Get psw from nvs: [HIDDEN]");
  } else {
    strlcpy(softap_psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(softap_psw));
    ESP_LOGI(TAG, "Get psw from nvs failed, set psw: [HIDDEN]");
  }

  esp_mesh_lite_set_softap_info(softap_ssid, softap_psw);
}

/* ═══════════════════════════════════════════════════════════════════
 * app_main
 * ═══════════════════════════════════════════════════════════════════ */
void app_main(void) {
  esp_log_level_set("*", ESP_LOG_INFO);

  esp_storage_init();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_bridge_create_all_netif();
  wifi_init();

  esp_mesh_lite_config_t mesh_lite_config = ESP_MESH_LITE_DEFAULT_INIT();
  mesh_lite_config.join_mesh_ignore_router_status = true;
#if CONFIG_MESH_ROOT
  mesh_lite_config.join_mesh_without_configured_wifi = false;
#else
  mesh_lite_config.join_mesh_without_configured_wifi = true;
#endif
  esp_mesh_lite_init(&mesh_lite_config);
  app_wifi_set_softap_info();

#if CONFIG_MESH_ROOT
  ESP_LOGI(TAG, "Role: ROOT (level 1)");
  esp_mesh_lite_set_allowed_level(1);
  s_nodes_mutex = xSemaphoreCreateMutex();
  configASSERT(s_nodes_mutex);
#else
  ESP_LOGI(TAG, "Role: NODE (level 2+)");
  esp_mesh_lite_set_disallowed_level(1);
#endif

  esp_mesh_lite_start();

  /* ── Queues ── */
  s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(mesh_msg_t));
  s_ack_queue = xQueueCreate(ACK_QUEUE_DEPTH, sizeof(mesh_msg_t));
  configASSERT(s_tx_queue && s_ack_queue);
#if !CONFIG_MESH_ROOT
  s_rx_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(mesh_msg_t));
  s_fwd_ack_queue = xQueueCreate(ACK_QUEUE_DEPTH, sizeof(mesh_msg_t));
  configASSERT(s_rx_queue && s_fwd_ack_queue);
#endif

  /* ── Tasks ── */
  xTaskCreate(blink_task, "blink", 2048, NULL, 3, &s_blink_handle);

#if CONFIG_MESH_ROOT
  esp_wifi_get_mac(WIFI_IF_STA, root_mac);
  gpio_init_all();
  xTaskCreate(root_send_task, "root_send", 3072, NULL, 5, NULL);
  xTaskCreate(button_pressed, "button_pressed", 4096, NULL, 5, NULL);
  xTaskCreate(root_ack_rx_task, "root_ack_rx", 4096, NULL, 7, NULL);
  xTaskCreate(root_ack_process_task, "root_ack_proc", 4096, NULL, 6, NULL);
#else
  xTaskCreate(node_rx_task, "node_rx", 4096, NULL, 7, NULL);
  xTaskCreate(node_forward_task, "node_fwd", 3072, NULL, 6, NULL);
  xTaskCreate(node_ack_rx_task, "node_ack_rx", 3072, NULL, 7, NULL);
  xTaskCreate(node_ack_fwd_task, "node_ack_fwd", 4096, NULL, 5, NULL);
#endif

  // TimerHandle_t timer = xTimerCreate("sys_info", pdMS_TO_TICKS(10000),
  // pdTRUE,
  //                                    NULL, print_system_info_timercb);
  // xTimerStart(timer, 0);

  ESP_LOGI(TAG, "Startup complete. Heap: %" PRIu32, esp_get_free_heap_size());
}
