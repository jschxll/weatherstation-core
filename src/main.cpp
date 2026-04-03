#include <Arduino.h>
#include <RTClib.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_BME280.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "HTTPUpdate.h"

#define TIME_TO_SLEEP 600000000ULL

struct periodic_data
{
  char time[40];
  float temp;
  float hum;
  float pressure;
};

RTC_DATA_ATTR uint32_t rtc_magic = 0xDEADBEEF;
RTC_DATA_ATTR int boot_count = 0;
RTC_DATA_ATTR periodic_data cached_measurements[5];
RTC_DATA_ATTR int measurement_index = 0;

RTC_DS3231 rtc;

Adafruit_BME280 bme;
Adafruit_Sensor *bme_temp = bme.getTemperatureSensor();
Adafruit_Sensor *bme_pressure = bme.getPressureSensor();
Adafruit_Sensor *bme_humidity = bme.getHumiditySensor();

const char *SSID = "SSID";
const char *PASSWORD = "PASSWORD";

WiFiClient client;
PubSubClient mqtt_client(client);
IPAddress mqtt_server(192, 168, 178, 120);

Preferences configPrefs;

void reconnect(const char *topic, const char *message)
{
  int retry = 0;
  while (!mqtt_client.connected() && retry < 3)
  {
    Serial.print("Attempting MQTT connection...");
    if (mqtt_client.connect("wetterstation"))
    {
      Serial.println(" connected");
      mqtt_client.publish(topic, message);
      delay(500);
      mqtt_client.disconnect();
      return;
    }
    else
    {
      Serial.print(" failed, rc=");
      Serial.print(mqtt_client.state());
      Serial.println(" trying again in 2 seconds");
      delay(2000);
    }
    retry++;
  }
}

double calculate_heat_index(float temp, float rel_humidity)
{
  double c1 = -8.784695;
  double c2 = 1.61139411;
  double c3 = 2.338549;
  double c4 = -0.14611605;
  double c5 = -0.012308094;
  double c6 = -0.016424828;
  double c7 = 0.002211732;
  double c8 = 0.00072546;
  double c9 = -0.000003582;

  return c1 + (c2 * temp) + (c3 * rel_humidity) + (c4 * temp * rel_humidity) +
         (c5 * (temp * temp)) + (c6 * (rel_humidity * rel_humidity)) +
         (c7 * (temp * temp) * rel_humidity) +
         (c8 * temp * (rel_humidity * rel_humidity)) + (c9 * (temp * temp) * (rel_humidity * rel_humidity));
}

float get_wind_speed()
{
  HTTPClient http;
  const char *url = "https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&models=icon_seamless&current=wind_speed_10m";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int resp_code = http.GET();

  if (resp_code != 200)
  {
    Serial.print("wind-speed-request has failed, code: ");
    Serial.print(resp_code);
    http.end();
    return -1;
  }

  String raw_json = http.getString();
  JsonDocument wind_speed_data;
  DeserializationError failed_deserialization = deserializeJson(wind_speed_data, raw_json);

  if (failed_deserialization.code() != DeserializationError::Ok)
  {
    Serial.print("Failed deserialization of json, code: ");
    Serial.print(failed_deserialization.code());
    return -1;
  }

  // Returns windspeed in km/h at 10m
  return wind_speed_data["current"]["wind_speed_10m"];
}

double calculate_wind_chill(float temp)
{
  float wind_speed = get_wind_speed();
  double wind_chill = 0;
  if (wind_speed != -1)
  {
    wind_chill = 13.12 + 0.6215 * temp + (0.3965 * temp - 11.37) * pow(wind_speed, 0.16);
  }

  return wind_chill;
}

void save_config_data()
{
  configPrefs.begin("rtc_config_data", false);

  // save rtc data that would be lost during restart
  configPrefs.putBool("restarted", true);
  configPrefs.putInt("boot_count", boot_count);
  configPrefs.putBytes("data_cache", cached_measurements, sizeof(cached_measurements));
  configPrefs.putInt("cache_index", measurement_index);
  configPrefs.end();
}

void save_current_firmware_version(const char *version)
{
  configPrefs.begin("current_version", false);
  configPrefs.putString("v", version);
  configPrefs.end();
}

void performOTAUpdate(WiFiClient client, const char *current_version)
{
  Serial.println("Starting OTA update check...");
  Serial.println("Connecting to update server at dietpi:3300...");

  // Step 1: Check for updates and get the new version from headers
  HTTPClient http;
  char check_url[256];
  snprintf(check_url, sizeof(check_url), "http://dietpi:3300/update?version=%s", current_version);

  http.begin(client, check_url);
  
  const char* headerKeys[] = {"x-version", "x-md5"};
  http.collectHeaders(headerKeys, 2);
  
  int http_code = http.GET();

  char new_version[32] = {0};
  bool update_available = false;

  if (http_code == 200)
  {
    String version_header = http.header("x-version");
    Serial.println(version_header);
    if (version_header.length() > 0)
    {
      strncpy(new_version, version_header.c_str(), sizeof(new_version) - 1);
      new_version[sizeof(new_version) - 1] = '\0';
      Serial.println("New version available: " + String(new_version));
      update_available = true;
    }
    else
    {
      Serial.println("Warning: No X-Version header in response");
    }
  }
  else if (http_code == 304)
  {
    Serial.println("No update available (304 Not Modified)");
    http.end();
    reconnect("wetterstation/otaUpdate", "No update available");
    return;
  }
  else
  {
    Serial.printf("Update check failed with HTTP code: %d\n", http_code);
    http.end();
    char message[256];
    snprintf(message, sizeof(message), "Update check failed: HTTP %d", http_code);
    reconnect("wetterstation/otaUpdate", message);
    return;
  }

  http.end();

  // Step 2: If update is available, perform the firmware update
  if (update_available)
  {
    Serial.println("Performing firmware update...");

    httpUpdate.rebootOnUpdate(true);

    httpUpdate.onStart([]() { 
      Serial.println("OTA Update started"); 
    });

    httpUpdate.onProgress([](int progress, int total) {
      Serial.printf("OTA Update progress: %d%%\n", (progress * 100) / total);
    });

    httpUpdate.onError([](int error) {
      Serial.printf("OTA Update error: %s\n", httpUpdate.getLastErrorString().c_str());
    });

    httpUpdate.onEnd([&]() {
      Serial.println("Update finished!");
      save_current_firmware_version(new_version);
      Serial.println("New version number saved");
    });

    t_httpUpdate_return ret = httpUpdate.update(client, "192.168.178.77", 3000, "/update", current_version);

    char message[256];
    switch (ret)
    {
    case HTTP_UPDATE_FAILED:
      snprintf(message, sizeof(message), "Update failed (%d): %s", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
      Serial.println(message);
      reconnect("wetterstation/otaUpdate", message);
      break;
    case HTTP_UPDATE_OK:
      Serial.println("Update successful!");
      reconnect("wetterstation/otaUpdate", "Update went successful!");
      break;
    default:
      Serial.println("Unknown update result");
      break;
    }
  }

  save_config_data();
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nStarting setup...");

  rtc.begin();

  if (rtc_magic != 0xDEADBEEF)
  {
    Serial.println("First boot or RTC data invalid");
  }

  boot_count++;
  Serial.println("Boot count: " + String(boot_count));

  // Retrieve config data stored before restart if the esp had a failure
  configPrefs.begin("rtc_config_data", false);
  if (configPrefs.isKey("restarted") && configPrefs.getBool("restarted"))
  {
    boot_count = configPrefs.getInt("boot_count");
    configPrefs.getBytes("data_cache", cached_measurements, sizeof(cached_measurements));
    measurement_index = configPrefs.getInt("cache_index");
    configPrefs.putBool("restarted", false);
  }
  configPrefs.end();

  // Connect to WiFi
  WiFi.disconnect();
  delay(100);
  WiFi.begin(SSID, PASSWORD);
  int retry = 0;

  while (WiFi.status() != WL_CONNECTED && retry < 10)
  {
    delay(500);
    retry++;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.print("WiFi connection failed.");

    // Restart ESP after wifi connection failed ten times
    save_config_data();
    ESP.restart();
  }

  delay(2000);

  if (!bme.begin(0x76))
  {
    Serial.println("bme not found");
    const char *topic = "error";
    const char *msg = "bme280 malfunction: sensor not found.";
    reconnect(topic, msg);
    while (1)
      delay(10);
  }

  // Setup MQTT Client
  mqtt_client.setServer(mqtt_server, 1883);

  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  //rtc.adjust(DateTime(2026, 4, 3, 16, 59, 0));

  if (rtc.lostPower())
  {
    reconnect("wetterstation/rtc", "RTC lost power!");
  }

  // If there is no firmware version stored in the preferences, initialize it with the default version. This is needed for the OTA update to work, as it compares the current version with the version on the server to decide whether to update or not.
  configPrefs.begin("current_version", false);
  if (!configPrefs.isKey("v"))
  {
    save_current_firmware_version("1.0.0");
  }
  configPrefs.end();

  // Perform OTA update if there is a new version available on the server
  configPrefs.begin("current_version", true);
  char current_version[32];
  String version_str = configPrefs.getString("v", "1.0.0");
  strncpy(current_version, version_str.c_str(), sizeof(current_version) - 1);
  current_version[sizeof(current_version) - 1] = '\0';
  configPrefs.end();

  performOTAUpdate(client, current_version);

  JsonDocument json;
  periodic_data &current_data_cache = cached_measurements[measurement_index];

  DateTime now = rtc.now();
  String time = now.timestamp(DateTime::TIMESTAMP_FULL);

  strncpy(current_data_cache.time, time.c_str(), sizeof(current_data_cache.time) - 1);
  current_data_cache.time[sizeof(current_data_cache.time) - 1] = '\0';

  sensors_event_t temp_event, pressure_event, humidity_event;
  bme_temp->getEvent(&temp_event);
  bme_humidity->getEvent(&humidity_event);
  bme_pressure->getEvent(&pressure_event);

  float temp = temp_event.temperature;
  float hum = humidity_event.relative_humidity;
  float pressure = pressure_event.pressure;

  current_data_cache.temp = temp;
  current_data_cache.hum = hum;
  current_data_cache.pressure = pressure;

  measurement_index++;

  // Calculate the current human-perceived temperature, above 26.7°C (heat index) or below (wind chill) 10°C
  double perceived_temp;
  if (temp > 26.7)
  {
    perceived_temp = calculate_heat_index(temp, hum);
  }
  else if (temp <= 10)
  {
    perceived_temp = calculate_wind_chill(temp);
    if (perceived_temp == 0)
    {
      perceived_temp = temp;
    }
  }
  else
  {
    perceived_temp = temp;
  }

  json["time"] = time;
  json["data"]["temp"] = temp;
  json["data"]["perceived_temp"] = perceived_temp;
  json["data"]["hum"] = hum;
  json["data"]["pressure"] = pressure;

  if (!mqtt_client.connected())
  {
    char json_buf[120];
    const char *topic = "wetterstation/perioden_messung";
    serializeJson(json, json_buf);
    reconnect(topic, json_buf);
  }

  // After 1h
  if (boot_count >= 5)
  {
    boot_count = 0;
    measurement_index = 0;

    char start_time[40];
    char end_time[40];
    float avg_temp = 0;
    float avg_hum = 0;
    float avg_pressure = 0;

    for (int i = 0; i < sizeof(cached_measurements) / sizeof(periodic_data); i++)
    {
      if (i == 0)
        strcpy(start_time, cached_measurements[i].time);

      if (i == 4)
        strcpy(end_time, cached_measurements[i].time);

      avg_temp += cached_measurements[i].temp;
      avg_hum += cached_measurements[i].hum;
      avg_pressure += cached_measurements[i].pressure;
    }

    avg_temp /= 5;
    avg_hum /= 5;
    avg_pressure /= 5;

    JsonDocument hourly_data;

    hourly_data["time"]["start"] = start_time;
    hourly_data["time"]["end"] = end_time;
    hourly_data["data"]["average_temp"] = avg_temp;
    hourly_data["data"]["average_hum"] = avg_hum;
    hourly_data["data"]["average_pressure"] = avg_pressure;

    char hourly_data_buf[150];
    serializeJson(hourly_data, hourly_data_buf);
    reconnect("wetterstation/stuendliche_messung", hourly_data_buf);
  }

  delay(1000);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP);
  Serial.flush();
  delay(3000);
  esp_deep_sleep_start();
}

void loop()
{
}