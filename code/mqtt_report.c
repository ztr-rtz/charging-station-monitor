/* =====================================================================
 * mqtt_report.c - 上报域：paho-mqtt 连接 OneNET，从快照读数据上报
 * ---------------------------------------------------------------------
 * 基于 04mqtt/ 参考代码改造，适配网关架构：
 *   - 从 node_snapshot 读取滤波后物理量（决策 D9）
 *   - 组装 OneNET 物模型 JSON 上报
 *   - 断线指数退避重连（FR-M06）
 *   - QoS1 + 时间戳幂等去重（FR-M05，NFR-R03）
 *
 * 依赖：paho-mqtt-c 同步版库（-lpaho-mqtt3c）
 * ===================================================================== */
#include "mqtt_report.h"
#include "data_process.h"
#include "node_snapshot.h"
#include "mbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <MQTTClient.h>

/* --------- OneNET 配置（来自 app.conf，当前硬编码） --------- */
#define MQTT_BROKER     "tcp://mqtts.heclouds.com:1883"
#define MQTT_PRODUCT_ID "5gScvSra72"
#define MQTT_DEV_NAME   "imx6ull"
#define MQTT_CLIENTID   MQTT_DEV_NAME
#define MQTT_PASSWD     "version=2018-10-31&res=products%2F5gScvSra72%2Fdevices%2Fimx6ull&et=2102651038&method=sha256&sign=dtVQufcgwCrn5zmdmWLXFEtJAQY%3D"
#define MQTT_QOS        1       /* QoS1 至少一次（FR-M05） */
#define MQTT_TIMEOUT    10000L  /* 发布超时 10s */
#define MQTT_KEEPALIVE  60      /* 心跳 60s（FR-M08） */

/* 5s 聚合上报周期（FR-M03） */
#define REPORT_INTERVAL_S  5

/* 重连参数 */
#define RECONNECT_BASE_MS   1000    /* 退避基数 1s */
#define RECONNECT_MAX_MS    60000   /* 退避上限 60s */
#define RECONNECT_MAX_RETRY 20      /* 最大重试次数 */

/* --------- 静态变量 --------- */
static MQTTClient               g_client;
static volatile int             g_connected = 0;
static volatile MQTTClient_deliveryToken g_deliveredToken;
static int                      g_msgId = 100000;

static char g_topic_sub[256];   /* 订阅：回复主题 */
static char g_topic_pub[256];   /* 发布：属性上报主题 */

/* --------- 回调函数 --------- */
static void on_delivered(void *ctx, MQTTClient_deliveryToken dt)
{
    (void)ctx;
    g_deliveredToken = dt;
}

static int on_message(void *ctx, char *topicName, int topicLen,
                      MQTTClient_message *msg)
{
    (void)ctx; (void)topicLen;
    printf("[MQTT] 收到 %s: %.*s\n", topicName, msg->payloadlen,
           (char *)msg->payload);
    MQTTClient_freeMessage(&msg);
    MQTTClient_free(topicName);
    return 1;
}

static void on_connlost(void *ctx, char *cause)
{
    (void)ctx;
    printf("[MQTT] 连接丢失: %s\n", cause ? cause : "unknown");
    g_connected = 0;
}

/* --------- 指数退避重连（FR-M06） --------- */
static int mqtt_reconnect(void)
{
    int delay = RECONNECT_BASE_MS;
    int retry;

    for (retry = 0; retry < RECONNECT_MAX_RETRY && g_connected == 0; retry++)
    {
        printf("[MQTT] 重连第 %d 次，等待 %dms...\n", retry + 1, delay);
        usleep((useconds_t)delay * 1000);

        MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
        opts.keepAliveInterval = MQTT_KEEPALIVE;
        opts.cleansession     = 0;
        opts.username          = MQTT_PRODUCT_ID;
        opts.password          = MQTT_PASSWD;

        /* LWT 遗嘱消息（重连时也需要配置） */
        static char lwt_topic2[256];
        sprintf(lwt_topic2, "$sys/%s/%s/thing/property/post",
                MQTT_PRODUCT_ID, MQTT_DEV_NAME);
        static const char *lwt_payload2 = "{\"id\":\"0\",\"version\":\"1.0\","
            "\"params\":{\"online\":{\"value\":0}}}";
        MQTTClient_willOptions will_opts2 = MQTTClient_willOptions_initializer;
        will_opts2.topicName   = lwt_topic2;
        will_opts2.message     = lwt_payload2;
        will_opts2.qos         = MQTT_QOS;
        will_opts2.retained    = 1;
        opts.will              = &will_opts2;

        int rc = MQTTClient_connect(g_client, &opts);
        if (rc == MQTTCLIENT_SUCCESS)
        {
            g_connected = 1;
            MQTTClient_subscribe(g_client, g_topic_sub, MQTT_QOS);
            printf("[MQTT] 重连成功\n");
            return 0;
        }

        /* 指数退避 + 抖动（决策 D11） */
        delay *= 2;
        if (delay > RECONNECT_MAX_MS)
            delay = RECONNECT_MAX_MS;
        delay += (rand() % 1000);
    }

    printf("[MQTT] 重连失败，已达最大重试次数\n");
    return -1;
}

/* --------- 初始化 --------- */
static int mqtt_init_conn(void)
{
    /* 组装主题 */
    sprintf(g_topic_sub, "$sys/%s/%s/thing/property/post/reply",
            MQTT_PRODUCT_ID, MQTT_DEV_NAME);
    sprintf(g_topic_pub, "$sys/%s/%s/thing/property/post",
            MQTT_PRODUCT_ID, MQTT_DEV_NAME);

    int rc = MQTTClient_create(&g_client, MQTT_BROKER, MQTT_CLIENTID,
                               MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[MQTT] 创建客户端失败: %d\n", rc);
        return -1;
    }

    MQTTClient_setCallbacks(g_client, NULL, on_connlost, on_message, on_delivered);

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = MQTT_KEEPALIVE;
    opts.cleansession     = 0;          /* FR-M08: 持久会话，配合 LWT */
    opts.username          = MQTT_PRODUCT_ID;
    opts.password          = MQTT_PASSWD;

    /* LWT 遗嘱消息（FR-M08）：broker 检测到连接断开后自动发布 */
    static char lwt_topic[256];
    sprintf(lwt_topic, "$sys/%s/%s/thing/property/post",
            MQTT_PRODUCT_ID, MQTT_DEV_NAME);
    static const char *lwt_payload = "{\"id\":\"0\",\"version\":\"1.0\","
        "\"params\":{\"online\":{\"value\":0}}}";
    opts.will               = NULL;  /* paho 同步版用 willOptions */
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    will_opts.topicName   = lwt_topic;
    will_opts.message     = lwt_payload;
    will_opts.qos         = MQTT_QOS;
    will_opts.retained    = 1;       /* 保留消息，新订阅者能立即看到离线状态 */
    opts.will              = &will_opts;

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[MQTT] 首次连接失败: %d，进入退避重连\n", rc);
        return mqtt_reconnect();
    }

    g_connected = 1;
    MQTTClient_subscribe(g_client, g_topic_sub, MQTT_QOS);
    printf("[MQTT] 已连接 %s，订阅 %s\n", MQTT_BROKER, g_topic_sub);
    return 0;
}

/* --------- 发布一条属性上报 --------- */
static int mqtt_publish_node(int idx)
{
    NodeSnap snap;
    if (node_snapshot_get(idx, &snap) != 0)
        return -1;
    if (!snap.online || snap.last_update_ms == 0)
        return 0;   /* 离线节点不上报 */

    const EnvProc *p = &snap.proc;
    char payload[1024];

    /* OneNET 物模型 JSON：三节点×6属性，key = node{idx+1}_{attr} */
    int nodeId = idx + 1;
    int alarmFlag = (p->status != 0) ? 1 : 0;   /* 状态字非0→1，物模型限定 0~1 */
    sprintf(payload,
        "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
        "\"node%d_temp\":{\"value\":%.1f},"
        "\"node%d_humi\":{\"value\":%.1f},"
        "\"node%d_smoke\":{\"value\":%.0f},"
        "\"node%d_volt\":{\"value\":%.2f},"
        "\"node%d_current\":{\"value\":%.3f},"
        "\"node%d_alarm\":{\"value\":%d}}}"
        ,
        g_msgId++,
        nodeId, p->temp_c,
        nodeId, p->humid_pct,
        nodeId, p->smoke,
        nodeId, p->voltage_v,
        nodeId, p->current_a,
        nodeId, alarmFlag);

    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload    = payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos        = MQTT_QOS;
    msg.retained   = 0;

    MQTTClient_deliveryToken token;
    int rc = MQTTClient_publishMessage(g_client, g_topic_pub, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[MQTT] 发布失败: %d\n", rc);
        g_connected = 0;
        return -1;
    }

    MQTTClient_waitForCompletion(g_client, token, MQTT_TIMEOUT);
    return 0;
}

/* --------- 发布全节点上报 --------- */
static void mqtt_publish_all(void)
{
    int cnt = node_snapshot_count();
    for (int i = 0; i < cnt; i++)
    {
        mqtt_publish_node(i);
    }
}

/* --------- 断网缓存（占位，后续接 SQLite cache_queue） --------- */
static void mqtt_cache_enqueue(const char *payload)
{
    /* TODO: FR-M07 断网缓存
     *   - sqlite3_exec INSERT INTO cache_queue(node_id, ts, payload, retry_count)
     *   - 调用时机：mqtt_publish_node 发布失败时
     */
    (void)payload;
}

/* --------- 恢复补传（占位，后续接 SQLite cache_queue） --------- */
static void mqtt_replay_cache(void)
{
    /* TODO: FR-M07 恢复补传
     *   - SELECT FROM cache_queue ORDER BY ts ASC LIMIT 20（每轮限流 20 条）
     *   - 按 (node_id, ts) 幂等去重（FR-M05）
     *   - 发送成功后 DELETE
     *   - 清空后切回实时上报
     */
}

/* --------- 时间辅助 --------- */
static long mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* --------- MQTT 线程入口（5s 聚合上报 + 断网重连） --------- */
void *mqtt_thread(void *arg)
{
    mbox_t *m = (mbox_t *)arg;
    uint32_t type;
    uint16_t len;
    uint8_t  buf[MBOX_DATA_LEN];
    int      has_new = 0;         /* 有新数据待上报 */
    long     last_report = 0;     /* 上次上报时间（秒） */

    /* 初始化连接 */
    mqtt_init_conn();
    last_report = mono_sec();

    while (1)
    {
        /* 非阻塞收邮箱（无消息立即返回），检测新数据通知 */
        if (mbox_recv_nowait(m, &type, buf, &len) == 0)
        {
            has_new = 1;
        }

        /* 5s 聚合定时器（FR-M03）：到时间且有新数据才上报 */
        long now = mono_sec();
        if (g_connected && has_new && (now - last_report) >= REPORT_INTERVAL_S)
        {
            mqtt_publish_all();       /* 聚合全节点一次性上报 */
            mqtt_replay_cache();      /* 恢复后补传存量（占位） */
            has_new = 0;
            last_report = now;
        }

        /* 连接断开时尝试重连 */
        if (!g_connected)
        {
            mqtt_reconnect();
        }

        usleep(100 * 1000);   /* 100ms 轮询间隔，降低 CPU 占用 */
    }
    return NULL;
}

/* --------- 清理 --------- */
void mqtt_deinit(void)
{
    if (g_connected)
    {
        MQTTClient_disconnect(g_client, 10000);
    }
    MQTTClient_destroy(&g_client);
    g_connected = 0;
}