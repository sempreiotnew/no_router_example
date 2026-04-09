/*
 * SPDX-FileCopyrightText: 2026 Sempre IoT (São Paulo)  LTD
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
#define ACK_FMT                                                                \
  "A:seq=%" PRIu32 ":mac=%02x:%02x:%02x:%02x:%02x:%02x:rssi=%d:par=%02x:%02x:" \
  "%02x:%02x:%02x:%02x"

/* ─── Tuning ──────────────────────────────────────────────────────── */
#define MSG_MAX_LEN 128
#define RX_QUEUE_DEPTH 32
#define TX_QUEUE_DEPTH 8
#define ACK_QUEUE_DEPTH 64
#define LED_GPIO 2

/* Retry config */
#define ACK_WINDOW_MS 1500    /* wait this long before checking ACKs  */
#define MAX_RETRIES 10        /* retransmit up to N times per seq      */
#define RETRY_DELAY_MS 300    /* wait between retries                  */
#define SEND_INTERVAL_MS 3000 /* gap between new seq (after all ACKs)  */

#define MAX_TRACKED_NODES 32

#define BUTTON_GPIO GPIO_NUM_0

/* ─── Upstream alarm (NODE → ROOT) ───────────────────────────────── */
#define NODE_ALARM_PORT 3335
#define NODE_ACK_PORT 3336
#define UA_FMT                                                                 \
  "UA:ALARM:seq=%" PRIu32 ":mac=%02x:%02x:%02x:%02x:%02x:%02x:rssi=%d"
#define UAK_FMT "UAK:seq=%" PRIu32 ":mac=%02x:%02x:%02x:%02x:%02x:%02x"
#define PFX_UA "UA:"
#define PFX_UAK "UAK:"
#define NODE_ALARM_RETRIES 20
#define NODE_ALARM_RETRY_DELAY_MS 800
#define NODE_ALARM_ACK_WINDOW_MS 1200

static const char *TAG = "fire_alarm";

bool button_on_off = false;
bool is_alarm = false;
volatile bool led_state = false;
static bool s_wifi_connected = false;
#if !CONFIG_MESH_ROOT
static volatile bool s_node_ip_changed = false; /* triggers socket recreate  */
static volatile TickType_t s_node_last_ip_tick = 0; /* tick of last IP change */
static volatile bool s_upstream_alarm_retry =
    false; /* retry upstream alarm ASAP */
#endif

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
static QueueHandle_t s_node_alarm_trigger_queue = NULL;
static QueueHandle_t s_fwd_alarm_queue =
    NULL; /* child UA: msgs → relay upstream */
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
static uint32_t s_upstream_alarm_sent = 0;
static uint32_t s_upstream_alarm_acked_cnt = 0;
/* volatile: written by uak_rx task, read by alarm send task */
static volatile uint32_t s_node_alarm_seq = 0;
static volatile bool s_node_alarm_acked = false;
static esp_err_t udp_broadcast(const char *msg, uint16_t len);

#define MAX_CHILD_IPS 16
static uint32_t s_child_ips[MAX_CHILD_IPS];
static int s_child_ip_count = 0;
static SemaphoreHandle_t s_child_ips_mutex = NULL;
#endif

/* ═══════════════════════════════════════════════════════════════════
 * NODE — forward to direct children (hybrid broadcast + unicast)
 * ═══════════════════════════════════════════════════════════════════ */
#if !CONFIG_MESH_ROOT

/* Learn a direct child's IP from the source address of an incoming ACK. */
static void child_ip_learn(uint32_t ip) {
  if (!ip || !s_child_ips_mutex)
    return;
  xSemaphoreTake(s_child_ips_mutex, portMAX_DELAY);
  for (int i = 0; i < s_child_ip_count; i++)
    if (s_child_ips[i] == ip) {
      xSemaphoreGive(s_child_ips_mutex);
      return;
    }
  if (s_child_ip_count < MAX_CHILD_IPS)
    s_child_ips[s_child_ip_count++] = ip;
  xSemaphoreGive(s_child_ips_mutex);
}

/* Forward DATA to every direct child using a hybrid strategy:
 *   1. Broadcast  — fire-and-forget; covers new/reconnected nodes whose IP
 *                   is not yet cached (topology change, first boot).
 *   2. Unicast    — one socket per cached child IP; gets 802.11 MAC-layer
 *                   ACK + hardware retry so the frame is reliable.
 * Nodes that receive both copies dedup them via the 500 ms burst window
 * in node_rx_task.  The cache is cleared on STADISCONNECTED so stale IPs
 * are evicted when a child leaves; broadcast covers the gap until the
 * cache is rebuilt from the next ACK round. */
static int udp_unicast_children(const char *msg, uint16_t len) {
  wifi_sta_list_t sl = {0};
  esp_wifi_ap_get_sta_list(&sl);
  if (sl.num == 0)
    return 0; /* no children at all */

  /* Step 1: broadcast (covers everyone, no IP needed) */
  udp_broadcast(msg, len);

  /* Step 2: unicast to each cached IP */
  if (!s_child_ips_mutex)
    return 1;
  xSemaphoreTake(s_child_ips_mutex, portMAX_DELAY);
  int count = s_child_ip_count;
  uint32_t ips[MAX_CHILD_IPS];
  memcpy(ips, s_child_ips, count * sizeof(uint32_t));
  xSemaphoreGive(s_child_ips_mutex);

  int sent = 0;
  for (int i = 0; i < count; i++) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0)
      continue;
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(ALARM_DATA_PORT),
        .sin_addr.s_addr = ips[i],
    };
    if (sendto(sock, msg, len, 0, (struct sockaddr *)&dest, sizeof(dest)) > 0)
      sent++;
    close(sock);
  }
  return sent + 1; /* +1 for the broadcast */
}

#endif /* !CONFIG_MESH_ROOT */

/* ═══════════════════════════════════════════════════════════════════
 * ROOT — delivery tracking
 * ═══════════════════════════════════════════════════════════════════ */
#if CONFIG_MESH_ROOT

typedef struct {
  uint8_t mac[6];
  uint8_t parent_mac[6]; /* STA MAC of direct parent (bssid - 1 on ESP32) */
  int level;
  bool valid;
  int8_t rssi;  /* RSSI to direct parent, from last ACK */
  bool missing; /* true if last broadcast round was not ACKed by this node */
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

static const uint8_t s_zero_mac[6] = {0};

static int track_node_locked(const uint8_t mac[6], int level, int8_t rssi,
                             const uint8_t parent_mac[6]) {
  for (int i = 0; i < s_node_count; i++)
    if (memcmp(s_nodes[i].mac, mac, 6) == 0) {
      if (level > 0)
        s_nodes[i].level = level;
      if (rssi != 0)
        s_nodes[i].rssi = rssi;
      if (parent_mac && memcmp(parent_mac, s_zero_mac, 6) != 0)
        memcpy(s_nodes[i].parent_mac, parent_mac, 6);
      return i;
    }
  if (s_node_count >= MAX_TRACKED_NODES)
    return -1;
  memcpy(s_nodes[s_node_count].mac, mac, 6);
  memcpy(s_nodes[s_node_count].parent_mac, parent_mac ? parent_mac : s_zero_mac,
         6);
  s_nodes[s_node_count].level = level;
  s_nodes[s_node_count].valid = true;
  s_nodes[s_node_count].rssi = rssi;
  ESP_LOGI(TAG,
           "[ROOT] new node [%d] mac:" MACSTR "  parent:" MACSTR
           "  rssi=%ddBm  level=%d",
           s_node_count, MAC2STR(mac),
           MAC2STR(s_nodes[s_node_count].parent_mac), rssi, level);
  return s_node_count++;
}

static void record_ack(uint32_t seq, const uint8_t mac[6], int8_t rssi,
                       const uint8_t parent_mac[6]) {
  if (!s_pending.active || s_pending.seq != seq)
    return;
  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
  int idx = track_node_locked(mac, 0, rssi, parent_mac);
  if (idx >= 0) {
    s_pending.acked[idx] = true;
    s_nodes[idx].missing = false; /* cleared on successful ACK */
  }
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

/* Recursive depth-first tree printer. Called with mutex already held. */
static void print_children_locked(const uint8_t parent_mac[6], int depth,
                                  uint32_t seq) {
  /* count direct children of parent_mac */
  int child_cnt = 0;
  for (int i = 0; i < s_node_count; i++)
    if (memcmp(s_nodes[i].parent_mac, parent_mac, 6) == 0)
      child_cnt++;
  if (child_cnt == 0)
    return;

  int drawn = 0;
  for (int i = 0; i < s_node_count; i++) {
    if (memcmp(s_nodes[i].parent_mac, parent_mac, 6) != 0)
      continue;
    drawn++;
    const char *branch = (drawn < child_cnt) ? "├──" : "└──";

    /* indent: 4 spaces per depth level */
    char indent[64] = "";
    for (int d = 0; d < depth; d++)
      strncat(indent, "    ", sizeof(indent) - strlen(indent) - 1);

    int lvl = s_nodes[i].level ? s_nodes[i].level : depth + 2;

    if (s_pending.active && s_pending.seq == seq) {
      bool acked = s_pending.acked[i];
      if (acked)
        ESP_LOGI(TAG,
                 "  %s%s [L%d] " MACSTR "  rssi=%ddBm  via=" MACSTR "  \u2713",
                 indent, branch, lvl, MAC2STR(s_nodes[i].mac),
                 (int)s_nodes[i].rssi, MAC2STR(parent_mac));
      else
        ESP_LOGW(TAG,
                 "  %s%s [L%d] " MACSTR "  rssi=%ddBm  via=" MACSTR
                 "  \u2717 MISSING",
                 indent, branch, lvl, MAC2STR(s_nodes[i].mac),
                 (int)s_nodes[i].rssi, MAC2STR(parent_mac));
    } else if (s_nodes[i].missing) {
      ESP_LOGW(TAG,
               "  %s%s [L%d] " MACSTR "  rssi=%ddBm  via=" MACSTR
               "  \u2717 MISSING",
               indent, branch, lvl, MAC2STR(s_nodes[i].mac),
               (int)s_nodes[i].rssi, MAC2STR(parent_mac));
    } else {
      ESP_LOGI(TAG, "  %s%s [L%d] " MACSTR "  rssi=%ddBm  via=" MACSTR, indent,
               branch, lvl, MAC2STR(s_nodes[i].mac), (int)s_nodes[i].rssi,
               MAC2STR(parent_mac));
    }

    /* recurse into this node's children */
    print_children_locked(s_nodes[i].mac, depth + 1, seq);
  }
}

static void print_tree(uint32_t seq) {
  uint8_t my_root_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, my_root_mac);

  /* Enrich levels from mesh node list (best-effort, no override) */
  uint32_t mesh_size = 0;
  const node_info_list_t *node = esp_mesh_lite_get_nodes_list(&mesh_size);

  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);

  while (node) {
    for (int i = 0; i < s_node_count; i++) {
      if (memcmp(s_nodes[i].mac, node->node->mac_addr, 6) == 0) {
        if (node->node->level > 0)
          s_nodes[i].level = node->node->level;
        break;
      }
    }
    node = node->next;
  }

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "Internet : %s", s_wifi_connected ? "connected" : "offline");
  ESP_LOGI(TAG, "  MESH TOPOLOGY  seq=%" PRIu32 "  (%d nodes)", seq,
           s_node_count);
  ESP_LOGI(TAG, "  [L1] " MACSTR "  <ROOT>", MAC2STR(my_root_mac));

  /* Main tree: depth-first walk from root */
  print_children_locked(my_root_mac, 0, seq);

  /* Orphan nodes: parent unknown (all-zero parent_mac).
   * These are nodes that only sent old-format ACKs without :par= */
  bool any_orphan = false;
  for (int i = 0; i < s_node_count; i++) {
    if (memcmp(s_nodes[i].parent_mac, s_zero_mac, 6) != 0)
      continue;
    if (!any_orphan) {
      ESP_LOGW(TAG, "  [parent unknown — old firmware or first seen]");
      any_orphan = true;
    }
    int lvl = s_nodes[i].level ? s_nodes[i].level : 0;
    if (s_pending.active && s_pending.seq == seq) {
      bool acked = s_pending.acked[i];
      if (acked)
        ESP_LOGI(TAG, "  └── [L%d?] " MACSTR "  rssi=%ddBm  \u2713", lvl,
                 MAC2STR(s_nodes[i].mac), (int)s_nodes[i].rssi);
      else
        ESP_LOGW(TAG, "  └── [L%d?] " MACSTR "  rssi=%ddBm  \u2717 MISSING",
                 lvl, MAC2STR(s_nodes[i].mac), (int)s_nodes[i].rssi);
    } else if (s_nodes[i].missing) {
      ESP_LOGW(TAG, "  └── [L%d?] " MACSTR "  rssi=%ddBm  \u2717 MISSING", lvl,
               MAC2STR(s_nodes[i].mac), (int)s_nodes[i].rssi);
    } else {
      ESP_LOGI(TAG, "  └── [L%d?] " MACSTR "  rssi=%ddBm", lvl,
               MAC2STR(s_nodes[i].mac), (int)s_nodes[i].rssi);
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

/* Broadcast on a specific port (used for UAK downstream) */
static esp_err_t udp_broadcast_port(const char *msg, uint16_t len,
                                    uint16_t port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
    return ESP_FAIL;
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  struct sockaddr_in dest = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = get_ap_broadcast(),
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
    /* Burst: send 3× with 80 ms gaps to survive transient lwIP misses */
    bool any_ok = false;
    for (int burst = 0; burst < 3; burst++) {
      if (burst > 0)
        vTaskDelay(pdMS_TO_TICKS(80));
      if (udp_broadcast(msg.data, msg.len) == ESP_OK)
        any_ok = true;
    }
    if (any_ok) {
      s_tx_ok++;
      ESP_LOGI(TAG, "[ROOT-SEND] TX (burst×3)  '%s'  (total_sent=%" PRIu32 ")",
               msg.data, s_tx_ok);
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
    unsigned int m[6] = {0}, p[6] = {0};
    int rssi_val = 0;

    /* Try full format (14 fields: seq+mac+rssi+par).
     * Fall back to rssi-only (8) or legacy no-rssi (7). */
    int fields = sscanf(msg.data,
                        "A:seq=%" PRIu32 ":mac=%x:%x:%x:%x:%x:%x:rssi=%d"
                        ":par=%x:%x:%x:%x:%x:%x",
                        &seq, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5],
                        &rssi_val, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]);
    if (fields < 7) {
      ESP_LOGE(TAG, "Error, the data is malformed");
      continue; /* malformed */
    }

    uint8_t mac[6], par[6];
    for (int i = 0; i < 6; i++) {
      mac[i] = (uint8_t)m[i];
      par[i] = (fields >= 14) ? (uint8_t)p[i] : 0;
    }
    if (fields < 8)
      rssi_val = 0; /* old firmware, no RSSI */

    record_ack(seq, mac, (int8_t)rssi_val, par);

    int acked = count_acked();
    int missing = s_node_count - acked;
    ESP_LOGI(TAG,
             "[ROOT-ACK] seq=%" PRIu32 "  from " MACSTR
             "  rssi=%ddBm  par=" MACSTR "  acked=%d/%d  missing=%d",
             seq, MAC2STR(mac), rssi_val, MAC2STR(par), acked, s_node_count,
             missing);
  }
}

/* ROOT — upstream alarm receiver: listens for UA: messages from nodes */
static void root_upstream_rx_task(void *arg) {
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
      .sin_port = htons(NODE_ALARM_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[ROOT-UP] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[ROOT-UP] upstream alarm listener on port %d",
           NODE_ALARM_PORT);

  char buf[MSG_MAX_LEN];
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);

  while (1) {
    int len = recvfrom(sock, buf, MSG_MAX_LEN - 1, 0, (struct sockaddr *)&src,
                       &src_len);
    if (len <= 0)
      continue;
    buf[len] = '\0';

    char src_ip[16];
    inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    ESP_LOGW(TAG, "[ROOT-UP] raw RX from=%s  '%s'", src_ip, buf);

    if (strncmp(buf, PFX_UA, strlen(PFX_UA)) != 0) {
      ESP_LOGW(TAG, "[ROOT-UP] unknown prefix, ignored");
      continue;
    }

    uint32_t seq = 0;
    unsigned int m[6] = {0};
    int rssi_val = 0;
    int parsed =
        sscanf(buf, "UA:ALARM:seq=%" PRIu32 ":mac=%x:%x:%x:%x:%x:%x:rssi=%d",
               &seq, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &rssi_val);
    if (parsed != 8) {
      ESP_LOGE(TAG, "[ROOT-UP] sscanf failed (got %d/8 fields)", parsed);
      continue;
    }
    {
      uint8_t mac[6];
      for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)m[i];

      /* ── Special ROOT log ── */
      ESP_LOGE(TAG, "");
      ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      ESP_LOGE(TAG, "!!  [ROOT] NODE ALARM RECEIVED                  !!");
      ESP_LOGE(TAG, "!!  Sender MAC : " MACSTR, MAC2STR(mac));
      ESP_LOGE(TAG, "!!  RSSI       : %ddBm (to parent)", rssi_val);
      ESP_LOGE(TAG, "!!  seq        : %" PRIu32, seq);
      ESP_LOGE(TAG, "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
      ESP_LOGE(TAG, "");

      /* Register node with RSSI so topology shows it */
      xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
      track_node_locked(mac, 0, (int8_t)rssi_val, NULL);
      xSemaphoreGive(s_nodes_mutex);

      /* Send UAK back downstream (broadcast on AP) */
      char ack[MSG_MAX_LEN];
      uint16_t ack_len =
          (uint16_t)snprintf(ack, sizeof(ack), UAK_FMT, seq, mac[0], mac[1],
                             mac[2], mac[3], mac[4], mac[5]);

      for (int t = 0; t < 3; t++) {
        if (udp_broadcast_port(ack, ack_len, NODE_ACK_PORT) == ESP_OK) {
          ESP_LOGI(TAG, "[ROOT-UP] UAK sent  '%s'", ack);
          break;
        }
        ESP_LOGW(TAG, "[ROOT-UP] UAK send retry %d/3", t + 1);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }
}

#endif /* CONFIG_MESH_ROOT */

/* ═══════════════════════════════════════════════════════════════════
 * NODE — RX task (prio 7, never stalls)
 * ═══════════════════════════════════════════════════════════════════ */
#if !CONFIG_MESH_ROOT
static int node_rx_open_socket(void) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0)
    return -1;
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  /* Short recv timeout so the IP-change check can wake up the loop.
   * 1 s is fine — at this frequency lwIP socket degradation does not occur. */
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(ALARM_DATA_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    close(sock);
    return -1;
  }
  return sock;
}

static void node_rx_task(void *arg) {
  int sock = -1;
  while (sock < 0) {
    sock = node_rx_open_socket();
    if (sock < 0)
      vTaskDelay(pdMS_TO_TICKS(500));
  }
  ESP_LOGI(TAG, "[NODE-RX] ready  level=%d", esp_mesh_lite_get_level());

  mesh_msg_t msg;
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);

  /* Burst dedup state — keyed on seq+alarm_state, time-bounded */
  uint32_t rx_last_seq = UINT32_MAX;
  bool rx_last_alarm = false;
  TickType_t rx_last_tick = 0;

  while (1) {
    /* Recreate socket after IP change so lwIP interface binding is fresh */
    if (s_node_ip_changed) {
      s_node_ip_changed = false;
      ESP_LOGI(TAG, "[NODE-RX] IP changed — recreating socket");
      close(sock);
      sock = -1;
      vTaskDelay(pdMS_TO_TICKS(200)); /* let lwIP settle */
      while (sock < 0) {
        sock = node_rx_open_socket();
        if (sock < 0)
          vTaskDelay(pdMS_TO_TICKS(500));
      }
      ESP_LOGI(TAG, "[NODE-RX] socket recreated  level=%d",
               esp_mesh_lite_get_level());
    }

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

    /* Quick burst dedup before enqueue: parse seq+state, drop copies seen
     * within 500ms (covers the 3-burst window of 160ms). Retries arrive
     * after ACK_WINDOW (600ms) and pass through normally. */
    uint32_t rx_seq = 0;
    char rx_state[6] = {0};
    bool rx_parsed = (sscanf(msg.data, "D:ALARM:%5[^:]:seq=%" PRIu32, rx_state,
                             &rx_seq) == 2);
    bool rx_alarm = (strcmp(rx_state, "true") == 0);
    if (rx_parsed && rx_seq == rx_last_seq && rx_alarm == rx_last_alarm &&
        (xTaskGetTickCount() - rx_last_tick) < pdMS_TO_TICKS(500)) {
      ESP_LOGD(TAG, "[NODE-RX] burst dup seq=%" PRIu32 " dropped", rx_seq);
      continue;
    }
    if (rx_parsed) {
      rx_last_seq = rx_seq;
      rx_last_alarm = rx_alarm;
      rx_last_tick = xTaskGetTickCount();
    }

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
    child_ip_learn(src.sin_addr.s_addr);
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
  bool last_alarm_state = false;
  TickType_t last_ack_tick = 0; /* tick when we last ACKed last_seq */

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

    /* Burst dedup: same seq+state AND we ACKed it recently (<500ms) → silent
     * drop. Keying on both seq and alarm_state ensures a state change
     * (true→false) with the same seq (shouldn't happen after the static seq
     * fix, but guards against old firmware on root). */
    if (parsed && seq == last_seq && is_alarm == last_alarm_state &&
        (xTaskGetTickCount() - last_ack_tick) < pdMS_TO_TICKS(500)) {
      ESP_LOGD(TAG, "[NODE-FWD] burst dup seq=%" PRIu32 " dropped", seq);
      continue;
    }

    bool is_dup = (parsed && seq == last_seq &&
                   is_alarm == last_alarm_state); /* retry, not burst copy */

    if (!is_dup) {
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
      if (parsed) {
        last_seq = seq;
        last_alarm_state = is_alarm;
      }

      /* Burst forward 3× (mirrors root burst) — gives L3+ nodes 3 chances
       * per retry cycle. node_rx_task dedup on the receiver drops copies 2&3.
       */
      int child_sent = 0;
      for (int fwd_burst = 0; fwd_burst < 3; fwd_burst++) {
        if (fwd_burst > 0)
          vTaskDelay(pdMS_TO_TICKS(80));
        int r = udp_unicast_children(msg.data, msg.len);
        if (r > 0)
          child_sent = r;
      }
      if (child_sent > 0) {
        s_fwd_ok++;
        ESP_LOGI(TAG,
                 "[NODE-FWD] FWD OK  seq=%" PRIu32
                 "  unicast_to=%d  (fwd=%" PRIu32 ")",
                 seq, child_sent, s_fwd_ok);
      } else {
        /* No children connected yet or send failed — not an error if leaf */
        wifi_sta_list_t sl = {0};
        esp_wifi_ap_get_sta_list(&sl);
        if (sl.num > 0) {
          s_fwd_fail++;
          ESP_LOGW(TAG,
                   "[NODE-FWD] FWD FAIL  seq=%" PRIu32 "  (fail=%" PRIu32 ")",
                   seq, s_fwd_fail);
        }
      }
    } else {
      /* Retry dup (same seq, >500ms since last ACK): re-forward to children so
       * nodes that missed the first forward get another chance. Only burst dups
       * (<500ms) are dropped without forwarding. */
      ESP_LOGD(TAG, "[NODE-FWD] retry dup seq=%" PRIu32 " — re-fwd + re-ACK",
               seq);
      int child_sent = 0;
      for (int fwd_burst = 0; fwd_burst < 3; fwd_burst++) {
        if (fwd_burst > 0)
          vTaskDelay(pdMS_TO_TICKS(80));
        int r = udp_unicast_children(msg.data, msg.len);
        if (r > 0)
          child_sent = r;
      }
      if (child_sent > 0) {
        s_fwd_ok++;
        ESP_LOGI(TAG,
                 "[NODE-FWD] FWD OK  seq=%" PRIu32
                 "  unicast_to=%d  (fwd=%" PRIu32 ")",
                 seq, child_sent, s_fwd_ok);
      } else {
        wifi_sta_list_t sl = {0};
        esp_wifi_ap_get_sta_list(&sl);
        if (sl.num > 0) {
          s_fwd_fail++;
          ESP_LOGW(TAG,
                   "[NODE-FWD] FWD FAIL  seq=%" PRIu32 "  (fail=%" PRIu32 ")",
                   seq, s_fwd_fail);
        }
      }
    }

    /* ACK upstream: for new seq OR retry (not burst dups) */
    if (parsed) {
      uint8_t my_mac[6];
      esp_wifi_get_mac(WIFI_IF_STA, my_mac);

      wifi_ap_record_t ap_info = {0};
      esp_wifi_sta_get_ap_info(&ap_info); /* RSSI + parent BSSID */

      /* On ESP32, AP_MAC = STA_MAC + 1 (last byte).
       * Derive parent STA MAC from the BSSID we connected to. */
      uint8_t par_sta[6];
      memcpy(par_sta, ap_info.bssid, 6);
      par_sta[5] = (par_sta[5] > 0) ? par_sta[5] - 1 : 0xFF;

      char ack[MSG_MAX_LEN];
      uint16_t ack_len = (uint16_t)snprintf(
          ack, sizeof(ack), ACK_FMT, seq, my_mac[0], my_mac[1], my_mac[2],
          my_mac[3], my_mac[4], my_mac[5], (int)ap_info.rssi, par_sta[0],
          par_sta[1], par_sta[2], par_sta[3], par_sta[4], par_sta[5]);

      /* After a topology change the STA netif briefly holds the old gateway
       * IP while DHCP negotiates with the new parent.  sendto() returns OK
       * even though the packet is routed to the wrong host.  Wait until the
       * gateway has been stable for ≥300 ms before sending the first ACK. */
      if (s_node_last_ip_tick > 0) {
        TickType_t elapsed = xTaskGetTickCount() - s_node_last_ip_tick;
        if (elapsed < pdMS_TO_TICKS(300))
          vTaskDelay(pdMS_TO_TICKS(300) - elapsed);
      }

      /* Retry ACK up to 5 times with 200 ms gaps */
      bool ack_ok = false;
      for (int t = 0; t < 5; t++) {
        if (udp_unicast_parent(ack, ack_len, ALARM_ACK_PORT) == ESP_OK) {
          s_ack_sent++;
          last_ack_tick = xTaskGetTickCount();
          ESP_LOGI(TAG, "[NODE-FWD] ACK sent  '%s'  (ack_sent=%" PRIu32 ")",
                   ack, s_ack_sent);
          ack_ok = true;
          break;
        }
        ESP_LOGW(TAG, "[NODE-FWD] ACK retry %d/5", t + 1);
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      if (!ack_ok)
        ESP_LOGE(TAG, "[NODE-FWD] ACK FAILED all retries  seq=%" PRIu32, seq);
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

// /* ═══════════════════════════════════════════════════════════════════
//  * NODE — GPIO0 button → trigger upstream alarm
//  * ═══════════════════════════════════════════════════════════════════ */
// static void node_button_task(void *arg) {
//   gpio_config_t btn_cfg = {
//       .pin_bit_mask = 1ULL << BUTTON_GPIO,
//       .mode = GPIO_MODE_INPUT,
//       .pull_up_en = GPIO_PULLUP_ENABLE,
//   };
//   gpio_config(&btn_cfg);

//   int last = 1;
//   while (1) {
//     int cur = gpio_get_level(BUTTON_GPIO);
//     if (last == 1 && cur == 0) { /* falling edge = press */
//       uint8_t trig = 1;
//       xQueueSend(s_node_alarm_trigger_queue, &trig, 0);
//       ESP_LOGI(TAG, "[NODE-BTN] GPIO0 pressed → upstream alarm triggered");
//     }
//     last = cur;
//     vTaskDelay(pdMS_TO_TICKS(50));
//   }
// }

static void node_button_task(void *arg) {
  uint8_t trig = 1;

  while (1) {
    xQueueSend(s_node_alarm_trigger_queue, &trig, 0);
    ESP_LOGI(TAG, "[NODE-TASK] Periodic trigger → upstream alarm sent");

    vTaskDelay(pdMS_TO_TICKS(60000)); // 1 minute
  }
}

/* ═══════════════════════════════════════════════════════════════════
 * NODE — upstream alarm sender with 20 retries + ACK wait
 * ═══════════════════════════════════════════════════════════════════ */
static void node_upstream_alarm_task(void *arg) {
  uint8_t trig;
  uint32_t seq = 0;
  uint8_t my_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, my_mac);

  while (1) {
    if (xQueueReceive(s_node_alarm_trigger_queue, &trig, portMAX_DELAY) !=
        pdTRUE)
      continue;

    seq++;
    s_node_alarm_seq = seq;
    s_node_alarm_acked = false;
    s_upstream_alarm_sent++;

    wifi_ap_record_t ap_info = {0};
    esp_wifi_sta_get_ap_info(&ap_info); /* RSSI to our parent */

    char msg[MSG_MAX_LEN];
    uint16_t msg_len = (uint16_t)snprintf(
        msg, sizeof(msg), UA_FMT, seq, my_mac[0], my_mac[1], my_mac[2],
        my_mac[3], my_mac[4], my_mac[5], (int)ap_info.rssi);

    ESP_LOGW(TAG,
             "[NODE-ALARM] *** Button pressed! Sending upstream alarm"
             "  seq=%" PRIu32 "  mac=" MACSTR "  rssi=%ddBm ***",
             seq, MAC2STR(my_mac), (int)ap_info.rssi);

    bool acked = false;
    int attempt = 0;
    while (!acked) {
      attempt++;

      /* On IP change: wait for gw stability before sending */
      if (s_node_last_ip_tick > 0) {
        TickType_t elapsed = xTaskGetTickCount() - s_node_last_ip_tick;
        if (elapsed < pdMS_TO_TICKS(300))
          vTaskDelay(pdMS_TO_TICKS(300) - elapsed);
      }
      s_upstream_alarm_retry = false;

      /* Refresh RSSI on every attempt so the message stays current */
      esp_wifi_sta_get_ap_info(&ap_info);
      msg_len = (uint16_t)snprintf(msg, sizeof(msg), UA_FMT, seq, my_mac[0],
                                   my_mac[1], my_mac[2], my_mac[3], my_mac[4],
                                   my_mac[5], (int)ap_info.rssi);

      esp_err_t err = udp_unicast_parent(msg, msg_len, NODE_ALARM_PORT);
      if (err == ESP_OK) {
        ESP_LOGI(TAG, "[NODE-ALARM] sent  attempt=%d/%d  seq=%" PRIu32, attempt,
                 NODE_ALARM_RETRIES, seq);
      } else {
        ESP_LOGW(TAG, "[NODE-ALARM] send FAIL  attempt=%d/%d  seq=%" PRIu32,
                 attempt, NODE_ALARM_RETRIES, seq);
      }

      /* Wait for ACK — cut short if IP change signals immediate retry */
      for (int w = 0; w < (NODE_ALARM_ACK_WINDOW_MS / 100); w++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_node_alarm_acked && s_node_alarm_seq == seq)
          break;
        if (s_upstream_alarm_retry)
          break; /* IP changed — retry now */
      }

      if (s_node_alarm_acked && s_node_alarm_seq == seq) {
        acked = true;
        s_upstream_alarm_acked_cnt++;
        ESP_LOGI(TAG,
                 "[NODE-ALARM] *** ACK RECEIVED  seq=%" PRIu32
                 "  after %d attempt(s) ***",
                 seq, attempt);
        break;
      }

      ESP_LOGW(TAG, "[NODE-ALARM] no ACK yet, retry %d/%d  seq=%" PRIu32,
               attempt + 1, NODE_ALARM_RETRIES, seq);

      /* Short pause before next retry (skip if IP change pending) */
      if (!s_upstream_alarm_retry)
        vTaskDelay(pdMS_TO_TICKS(NODE_ALARM_RETRY_DELAY_MS));
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════
 * NODE — upstream alarm RX: pure blocking recvfrom (no SO_RCVTIMEO),
 *        mirrors node_ack_rx_task pattern which is proven to work for L3.
 *        Receives UA: from children, enqueues to s_fwd_alarm_queue.
 * ═══════════════════════════════════════════════════════════════════ */
static void node_alarm_rx_task(void *arg) {
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
  /* NO SO_RCVTIMEO — pure blocking, same as node_ack_rx_task */

  struct sockaddr_in bind_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(NODE_ALARM_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[NODE-ALARM-RX] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[NODE-ALARM-RX] ready on port %d", NODE_ALARM_PORT);

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

    if (strncmp(msg.data, PFX_UA, strlen(PFX_UA)) != 0)
      continue;

    char src_ip[16];
    inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
    ESP_LOGI(TAG, "[NODE-ALARM-RX] got UA from child %s  '%s'", src_ip,
             msg.data);

    if (xQueueSend(s_fwd_alarm_queue, &msg, 0) != pdTRUE)
      ESP_LOGW(TAG, "[NODE-ALARM-RX] queue full, dropped '%s'", msg.data);
  }
}

/* NODE — upstream alarm relay: stores last alarm, retries indefinitely until
 * a UAK propagates back (s_upstream_alarm_retry speeds up on IP change). */
static volatile bool s_fwd_alarm_uak_received = false;

static void node_alarm_fwd_task(void *arg) {
  mesh_msg_t pending;
  pending.len = 0;

  while (1) {
    /* Pick up new alarm (or replacement) from queue — non-blocking when
     * we already have a pending message to retry. */
    mesh_msg_t tmp;
    BaseType_t got = xQueueReceive(s_fwd_alarm_queue, &tmp,
                                   pending.len > 0 ? 0 : portMAX_DELAY);
    if (got == pdTRUE) {
      memcpy(&pending, &tmp, sizeof(pending));
      s_fwd_alarm_uak_received = false; /* new alarm resets UAK state */
    }

    if (pending.len == 0)
      continue;

    /* Stop retrying once a UAK has been received for this alarm */
    if (s_fwd_alarm_uak_received) {
      pending.len = 0;
      continue;
    }

    /* Wait for gw stability after IP change */
    if (s_node_last_ip_tick > 0) {
      TickType_t elapsed = xTaskGetTickCount() - s_node_last_ip_tick;
      if (elapsed < pdMS_TO_TICKS(300))
        vTaskDelay(pdMS_TO_TICKS(300) - elapsed);
    }
    s_upstream_alarm_retry = false;

    if (udp_unicast_parent(pending.data, pending.len, NODE_ALARM_PORT) ==
        ESP_OK) {
      ESP_LOGI(TAG, "[NODE-ALARM-FWD] relayed upstream  '%s'", pending.data);
    } else {
      ESP_LOGW(TAG, "[NODE-ALARM-FWD] relay retry %d/5", 1);
    }

    /* Wait before next retry — cut short on IP change or new queue item */
    for (int w = 0; w < 10; w++) { /* 10 × 200 ms = 2 s max wait */
      vTaskDelay(pdMS_TO_TICKS(200));
      if (s_upstream_alarm_retry)
        break;
      if (s_fwd_alarm_uak_received)
        break;
      if (uxQueueMessagesWaiting(s_fwd_alarm_queue) > 0)
        break;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════
 * NODE — UAK downstream receiver: receives root ACK (UAK:),
 *        signals own alarm task if MAC matches,
 *        forwards downstream so deeper nodes get their ACK too.
 * ═══════════════════════════════════════════════════════════════════ */
static void node_uak_rx_task(void *arg) {
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
      .sin_port = htons(NODE_ACK_PORT),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  while (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGW(TAG, "[NODE-UAK-RX] bind() retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "[NODE-UAK-RX] ready on port %d", NODE_ACK_PORT);

  uint8_t my_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, my_mac);

  char buf[MSG_MAX_LEN];
  struct sockaddr_in src;
  socklen_t src_len = sizeof(src);

  while (1) {
    int len = recvfrom(sock, buf, MSG_MAX_LEN - 1, 0, (struct sockaddr *)&src,
                       &src_len);
    if (len <= 0)
      continue;
    buf[len] = '\0';

    if (strncmp(buf, PFX_UAK, strlen(PFX_UAK)) != 0)
      continue;

    uint32_t seq = 0;
    unsigned int m[6] = {0};
    if (sscanf(buf, "UAK:seq=%" PRIu32 ":mac=%x:%x:%x:%x:%x:%x", &seq, &m[0],
               &m[1], &m[2], &m[3], &m[4], &m[5]) == 7) {
      uint8_t ack_mac[6];
      for (int i = 0; i < 6; i++)
        ack_mac[i] = (uint8_t)m[i];

      /* Any UAK means root received an upstream alarm — stop fwd retries */
      s_fwd_alarm_uak_received = true;

      /* Is this ACK for us? */
      if (memcmp(ack_mac, my_mac, 6) == 0) {
        ESP_LOGI(TAG, "[NODE-UAK-RX] *** ROOT ACK for US  seq=%" PRIu32 " ***",
                 seq);
        if (s_node_alarm_seq == seq)
          s_node_alarm_acked = true;
      } else {
        ESP_LOGI(TAG, "[NODE-UAK-RX] UAK for " MACSTR "  seq=%" PRIu32,
                 MAC2STR(ack_mac), seq);
      }

      /* Forward downstream so level-3+ nodes can receive their ACK */
      wifi_sta_list_t sl = {0};
      esp_wifi_ap_get_sta_list(&sl);
      if (sl.num > 0) {
        if (udp_broadcast_port(buf, (uint16_t)len, NODE_ACK_PORT) == ESP_OK) {
          ESP_LOGI(TAG, "[NODE-UAK-RX] UAK forwarded downstream  children=%d",
                   sl.num);
        } else {
          ESP_LOGW(TAG, "[NODE-UAK-RX] UAK fwd downstream FAIL");
        }
      }
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

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
      wifi_event_ap_staconnected_t *event =
          (wifi_event_ap_staconnected_t *)event_data;

      ESP_LOGI(TAG, "✅ Station joined: " MACSTR ", AID=%d",
               MAC2STR(event->mac), event->aid);
    }

    if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
      wifi_event_ap_stadisconnected_t *event =
          (wifi_event_ap_stadisconnected_t *)event_data;

      ESP_LOGW(TAG, "❌ Station left: " MACSTR, MAC2STR(event->mac));
#if !CONFIG_MESH_ROOT
      if (s_child_ips_mutex) {
        xSemaphoreTake(s_child_ips_mutex, portMAX_DELAY);
        s_child_ip_count = 0;
        memset(s_child_ips, 0, sizeof(s_child_ips));
        xSemaphoreGive(s_child_ips_mutex);
      }
#endif
    }
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    ESP_LOGI(TAG, "🔥 GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));

    s_wifi_connected = true;
#if !CONFIG_MESH_ROOT
    s_node_ip_changed = true;
    s_node_last_ip_tick = xTaskGetTickCount();
    s_upstream_alarm_retry = true;
#endif
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

static void log_mesh_json(uint32_t seq, bool delivered) {
  char buf[256];

  uint8_t root_mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, root_mac);

  int acked = count_acked();

  // Header (NOW includes root MAC)
  snprintf(buf, sizeof(buf),
           "{"
           "\"seq\":%" PRIu32 ","
           "\"root\":\"" MACSTR "\","
           "\"delivered\":%s,"
           "\"acked\":%d,"
           "\"total\":%d,"
           "\"retries\":%" PRIu32 ","
           "\"nodes\":[",
           seq, MAC2STR(root_mac), delivered ? "true" : "false", acked,
           s_node_count, s_retries);

  printf("%s", buf);

  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);

  for (int i = 0; i < s_node_count; i++) {
    char node_buf[256];

    snprintf(node_buf, sizeof(node_buf),
             "{"
             "\"mac\":\"" MACSTR "\","
             "\"layer\":%d,"
             "\"parent\":\"" MACSTR "\","
             "\"rssi\":%d,"
             "\"acked\":%s,"
             "\"missing\":%s"
             "}%s",
             MAC2STR(s_nodes[i].mac), s_nodes[i].level,
             MAC2STR(s_nodes[i].parent_mac), s_nodes[i].rssi,
             s_pending.acked[i] ? "true" : "false",
             s_nodes[i].missing ? "true" : "false",
             (i < s_node_count - 1) ? "," : "");

    printf("%s", node_buf);
  }

  xSemaphoreGive(s_nodes_mutex);

  printf("]}\n");
}
void send_broadcast() {
  static uint32_t seq = 0;
  seq++;
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

  /* ── Stamp persistent missing flag ── */
  xSemaphoreTake(s_nodes_mutex, portMAX_DELAY);
  for (int i = 0; i < s_node_count; i++) {
    if (!s_pending.acked[i])
      s_nodes[i].missing = true;
    /* missing=false already cleared in record_ack for acked nodes */
  }
  xSemaphoreGive(s_nodes_mutex);

  if (!delivered) {
    ESP_LOGE(TAG,
             "[ROOT-PROD] DELIVERY INCOMPLETE  seq=%" PRIu32
             "  acked=%d/%d after %d retries",
             seq, count_acked(), s_node_count, MAX_RETRIES);
  }

  /* ── Print tree with ACK status ── */
  print_tree(seq);
  log_mesh_json(seq, delivered);

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
  // esp_log_level_set("*", ESP_LOG_NONE); // disable all logs

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
  s_node_alarm_trigger_queue = xQueueCreate(4, sizeof(uint8_t));
  s_fwd_alarm_queue = xQueueCreate(ACK_QUEUE_DEPTH, sizeof(mesh_msg_t));
  s_child_ips_mutex = xSemaphoreCreateMutex();
  configASSERT(s_rx_queue && s_fwd_ack_queue && s_node_alarm_trigger_queue &&
               s_fwd_alarm_queue && s_child_ips_mutex);
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
  xTaskCreate(root_upstream_rx_task, "root_up_rx", 4096, NULL, 7, NULL);
#else
  xTaskCreate(node_rx_task, "node_rx", 4096, NULL, 7, NULL);
  xTaskCreate(node_forward_task, "node_fwd", 3072, NULL, 6, NULL);
  xTaskCreate(node_ack_rx_task, "node_ack_rx", 3072, NULL, 7, NULL);
  xTaskCreate(node_ack_fwd_task, "node_ack_fwd", 4096, NULL, 5, NULL);
  xTaskCreate(node_button_task, "node_btn", 2048, NULL, 5, NULL);
  xTaskCreate(node_upstream_alarm_task, "node_alarm", 4096, NULL, 6, NULL);
  xTaskCreate(node_alarm_rx_task, "node_alarm_rx", 4096, NULL, 7, NULL);
  xTaskCreate(node_alarm_fwd_task, "node_alarm_fwd", 4096, NULL, 6, NULL);
  xTaskCreate(node_uak_rx_task, "node_uak_rx", 4096, NULL, 7, NULL);
#endif

  // TimerHandle_t timer = xTimerCreate("sys_info", pdMS_TO_TICKS(10000),
  // pdTRUE,
  //                                    NULL, print_system_info_timercb);
  // xTimerStart(timer, 0);

  ESP_LOGI(TAG, "Startup complete. Heap: %" PRIu32, esp_get_free_heap_size());
}
