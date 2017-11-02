#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_event_loop.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/dns.h>

#include <aws_iot_mqtt_client_interface.h>
#include <aws_iot_version.h>
#include <aws_iot_config.h>
#include <aws_iot_log.h>

/*
 * GPIO configuration
 * RSTSW_GPIO: (Output) Pin that the gate of the reset switch n-channel MOSFET is connected to
 * PWRSW_GPIO: (Output) Pin that the gate of the power switch n-channel MOSFET is connected to
 * The positive voltage-biased power / reset switch connector of the mainboard should be connected
 * to the respective n-channel MOSFET's drain while the respective ground connector should be connected
 * to the MOSFET's source. Use a multimeter to check which of your mainboard's power / reset switch pins
 * is tied to ground and which one is the active-low input.
 * These pins are used to turn on / turn off / reset the computer.
 * PWRLED_GPIO: (Input) Pin that the power led indicator of the mainboard is connected to
 * This pin is used to get the computer's power status.
 */
#define RSTSW_GPIO 5
#define PWRSW_GPIO 18
#define PWRLED_GPIO 19

/*
 * MQTT topics to subscribe to and process
 * MQTT_TOPIC_RST: Reset switch topic, a '1' message on this channel will pull the reset
 * switch active-low input low for a short duration (TODO: currently not implemeneted)
 * MQTT_TOPIC_PWR: Power switch topic, a '1' message on this channel will pull the power
 * switch active-low input low for a short duration
 */
#define MQTT_TOPIC_ALL CONFIG_AWS_TOPIC_BASE "/+"
#define MQTT_TOPIC_RST CONFIG_AWS_TOPIC_BASE "/reset"
#define MQTT_TOPIC_PWR CONFIG_AWS_TOPIC_BASE "/power"

/*
 * A connection sequence of an Amazon Dash Button consists of multiple data packets
 * containing the dash button's MAC address as source / destination address in the WiFi
 * header.
 * Since we don't want the power switch input to be triggered multiple times, we ignore
 * any further occurences of the dash button's MAC address after the first occurence for
 * DASH_UNBLOCK_TIMEOUT milliseconds.
 */
#define DASH_UNBLOCK_TIMEOUT 4000

static const char *TAG = "alexa-power-button";

typedef struct {
	uint8_t bytes[6];
} MACAddr;

typedef struct {
	uint8_t framectl1;
	uint8_t framectl2;
	uint16_t duration;
	MACAddr destination;
	MACAddr source;
	MACAddr bssid;
	uint16_t seqctl;
	uint8_t payload[];
} IEEE80211Header;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/*
 * When the Amazon Dash Button is pressed, the corresponding MAC address may appear
 * as source / destination address in the WiFi header multiple times.
 * In order to make sure that the power button is only triggered once, a global variable
 * dash_button_blocked is created that is automatically reset after DASH_UNBLOCK_TIMEOUT.
 */
bool dash_button_blocked = false;

/*
 * Certificates (root CA, thing certificate + private key) are embedded in binary
 */
extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

static esp_err_t event_handler(void *ctx, system_event_t *event) {
	switch(event->event_id) {
	case SYSTEM_EVENT_STA_START:
		esp_wifi_connect();
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently
		   auto-reassociate. */
		esp_wifi_connect();
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
	return ESP_OK;
}

/*
 * MOSFET power / reset switch
 * Emulates push-and-release of power / reset switch
 */
static void switchInit(uint8_t gpio) {
	gpio_pad_select_gpio(gpio);
	gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
	gpio_set_level(gpio, 0);
}

static void switchPushAndRelease(uint8_t gpio) {
	gpio_set_level(gpio, 1);
	vTaskDelay(1000 / portTICK_RATE_MS);
	gpio_set_level(gpio, 0);
}

/*
 * MQTT message handler
 */
static void onMQTTMsg(AWS_IoT_Client *pClient, char *topic, uint16_t topic_len, IoT_Publish_Message_Params *params, void *pData) {
	ESP_LOGI(TAG, "MQTT: Got message, channel \"%.*s\", content \"%.*s\"", topic_len, topic, params->payloadLen, (char *)params->payload);

	uint8_t gpio = 0;
	if (strncmp(topic, MQTT_TOPIC_RST, topic_len) == 0)
		gpio = RSTSW_GPIO;
	else if (strncmp(topic, MQTT_TOPIC_PWR, topic_len) == 0)
		gpio = PWRSW_GPIO;
	else {
		ESP_LOGW(TAG, "MQTT: Unknown topic %.*s, ignoring", topic_len, topic);
		return;
	}

	if (strncmp(params->payload, "1", params->payloadLen) == 0)
		switchPushAndRelease(gpio);
	else {
		ESP_LOGW(TAG, "MQTT: Unknown content %.*s, ignoring", params->payloadLen, (char *) params->payload);
		return;
	}	
}

// Client should auto-reconnect, nothing to do here
static void onMQTTDisconnect(AWS_IoT_Client *pClient, void *data) {
	ESP_LOGW(TAG, "MQTT: Disconnected, auto-reconnecting");
}

static void MQTTPublish(AWS_IoT_Client *client, char *topic, char *msg) {
	IoT_Publish_Message_Params publishParams;
	publishParams.qos = QOS0;
	publishParams.payload = msg;
	publishParams.payloadLen = strlen(msg);
	publishParams.isRetained = 0;
	aws_iot_mqtt_publish(client, topic, strlen(topic), &publishParams);
}

/*
 * Connect to Amazon AWS IoT
 * and subscribe to relevant MQTT topics
 */
static void mqtt_task(void *param) {
	IoT_Error_t rc = FAILURE;

	ESP_LOGI(TAG, "MQTT: AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

	/*
	 * Initialize MQTT client with remote host + port and crypto
	 */
	IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
	mqttInitParams.enableAutoReconnect = false; // We enable this later below
	mqttInitParams.pHostURL = AWS_IOT_MQTT_HOST;
	mqttInitParams.port = AWS_IOT_MQTT_PORT;
	mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
	mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
	mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;
	mqttInitParams.disconnectHandler = onMQTTDisconnect;

	AWS_IoT_Client client;
	rc = aws_iot_mqtt_init(&client, &mqttInitParams);
	if (SUCCESS != rc) {
		ESP_LOGE(TAG, "MQTT: aws_iot_mqtt_init returned error : %d ", rc);
		abort();
	}

	/*
	 * Wait for WiFi Connection
	 */
	ESP_LOGI(TAG, "MQTT: Waiting for WiFi connection before attempting to connect to AWS IoT...");
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

	/*
	 * Connect to AWS IoT
	 */
	IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;
	connectParams.keepAliveIntervalInSec = 10;
	connectParams.pClientID = CONFIG_AWS_CLIENT_ID;
	connectParams.clientIDLen = (uint16_t) strlen(CONFIG_AWS_CLIENT_ID);

	ESP_LOGI(TAG, "MQTT: Connecting to AWS IoT...");
	do {
		rc = aws_iot_mqtt_connect(&client, &connectParams);
		if (SUCCESS != rc) {
			ESP_LOGE(TAG, "MQTT: Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
			vTaskDelay(1000 / portTICK_RATE_MS);
		}
	} while(SUCCESS != rc);

	/*
	 * Enable Auto Reconnect functionality.
	 */
	rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
	if (SUCCESS != rc) {
		ESP_LOGE(TAG, "MQTT: Unable to set Auto Reconnect to true - %d", rc);
		abort();
	}

	/*
	 * Subscribe to all relevant topics
	 */
	ESP_LOGI(TAG, "MQTT: Subscribing to %s", MQTT_TOPIC_ALL);

	rc = aws_iot_mqtt_subscribe(&client, MQTT_TOPIC_ALL, strlen(MQTT_TOPIC_ALL), QOS0, onMQTTMsg, NULL);
	if(SUCCESS != rc) {
		ESP_LOGE(TAG, "MQTT: Error subscribing: %d ", rc);
		abort();
	}

	/*
	 * Wait for incoming MQTT messages
	 */
	while ((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc)) {
		ESP_LOGI(TAG, "MQTT: Waiting for messages...");
		rc = aws_iot_mqtt_yield(&client, 3000);
	}

	ESP_LOGE(TAG, "MQTT: Terminated unexpectedly");
	abort();
}

/*
 * Amazon Dash Button handlers
 * Including some MAC address utilities
 */
static bool compareMACAddress(MACAddr mac1, MACAddr mac2) {
	for (uint8_t i = 0; i < 6; ++i)
		if (mac1.bytes[i] != mac2.bytes[i])
			return false;

	return true;
}

static void macAddrFromString(MACAddr *target, char *macstring) {
	char macstring_copy[18];
	memcpy(macstring_copy, macstring, 18);

	// Replace ':' with '\0' and check format
	for (uint8_t i = 0; i < 5; ++i) {
		if (macstring_copy[2 + i * 3] != ':') {
			ESP_LOGE(TAG, "DASH: Dash Button MAC Address invalid");
		} else {
			macstring_copy[2 + i * 3] = '\0';
		}
	}

	// Convert hex string to MACAddr
	for (uint8_t i = 0; i < 6; ++i)
		target->bytes[i] = strtol(&macstring_copy[i * 3], NULL, 16);
}

static void dashUnblockTimeout(TimerHandle_t xTimer) {
	dash_button_blocked = false;
}

/*
 * Sniff all WiFi traffic and watch out for the dash button's MAC address
 * as destination / source address in IEEE 802.11 header.
 */
static void dashButtonSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
	if (type == WIFI_PKT_DATA) {
		// Interpret buffer payload as IEEE 802.11 header
		IEEE80211Header *header = (IEEE80211Header*)(((wifi_promiscuous_pkt_t*)buf)->payload);

		// Compare IEEE 802.11 source and destination addresses with Dash Button MAC
		MACAddr dashMAC;
		macAddrFromString(&dashMAC, CONFIG_DASH_BUTTON_MAC);
		if ((compareMACAddress(header->source, dashMAC) || compareMACAddress(header->destination, dashMAC)) && !dash_button_blocked) {
			ESP_LOGI(TAG, "Amazon Dash Button press registered");
			dash_button_blocked = true;
			TimerHandle_t unblock_timer = xTimerCreate("dashUnblockTimeout", DASH_UNBLOCK_TIMEOUT / portTICK_RATE_MS, false, NULL, dashUnblockTimeout);
			xTimerStart(unblock_timer, 0);
			switchPushAndRelease(PWRSW_GPIO);
		}
	}
}

static void wifi_init() {
	/*
	 * Standard WiFi connection setup
	 */
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PASSWORD,
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	/*
	 * Enable packet sniffer for WiFi header MAC address detection for Amazon Dash Button
	 */
	const wifi_promiscuous_filter_t filt = {
		.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
	};

	ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&dashButtonSniffer));
}

void app_main() {
	wifi_init();
	switchInit(PWRSW_GPIO);
	switchInit(RSTSW_GPIO);
	xTaskCreatePinnedToCore(&mqtt_task, "mqtt_task", 36 * 1024, NULL, 5, NULL, 1);
}
