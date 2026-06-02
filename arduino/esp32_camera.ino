#include <SALAH_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

#include "esp_camera.h"
#include "img_converters.h"

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ===============================
// WIFI
// ===============================
const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

// ===============================
// SERVER
// ===============================
WebServer server(80);

// ===============================
// THINGSPEAK
// ===============================
const char* THINGSPEAK_API_KEY = "YOUR_THINGSPEAK_WRITE_API_KEY";
const char* THINGSPEAK_URL = "http://api.thingspeak.com/update";

#define THINGSPEAK_MIN_INTERVAL_MS 16000
uint32_t lastThingSpeakSend = 0;

// ===============================
// DECISION CONFIG
// ===============================
#define AUTH_THRESHOLD 0.70f
#define DOOR_OPEN_MS 3000
#define REQUIRED_AUTHORIZED_HITS 1

// ===============================
// STATE
// ===============================
float lastAuthorizedScore = 0.0f;
float lastNotAuthorizedScore = 0.0f;

String lastDecision = "WAITING";
String doorState = "CLOSED";
String modelMode = "float32";

int authorizedIndex = -1;
int notAuthorizedIndex = -1;

uint32_t eventId = 0;
uint32_t totalAuthorized = 0;
uint32_t totalRefused = 0;
uint32_t doorOpenedAt = 0;

uint8_t authorizedHits = 0;
uint8_t refusedHits = 0;

bool isBusy = false;
bool cameraReady = false;

// Debug values for interface
int dbgFrameWidth = 0;
int dbgFrameHeight = 0;
int dbgFrameFormat = 0;
int dbgFrameLength = 0;

// ===============================
// AI THINKER ESP32-CAM PINS
// ===============================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ===============================
// CAMERA CONFIG
// ===============================

#define EI_CAMERA_RAW_FRAME_BUFFER_COLS   320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS   240
#define EI_CAMERA_FRAME_BYTE_SIZE         3

static bool debug_nn = false;
static bool is_initialised = false;
static uint8_t *snapshot_buf = nullptr;

static camera_config_t camera_config = {
  .pin_pwdn = PWDN_GPIO_NUM,
  .pin_reset = RESET_GPIO_NUM,
  .pin_xclk = XCLK_GPIO_NUM,
  .pin_sscb_sda = SIOD_GPIO_NUM,
  .pin_sscb_scl = SIOC_GPIO_NUM,

  .pin_d7 = Y9_GPIO_NUM,
  .pin_d6 = Y8_GPIO_NUM,
  .pin_d5 = Y7_GPIO_NUM,
  .pin_d4 = Y6_GPIO_NUM,
  .pin_d3 = Y5_GPIO_NUM,
  .pin_d2 = Y4_GPIO_NUM,
  .pin_d1 = Y3_GPIO_NUM,
  .pin_d0 = Y2_GPIO_NUM,

  .pin_vsync = VSYNC_GPIO_NUM,
  .pin_href = HREF_GPIO_NUM,
  .pin_pclk = PCLK_GPIO_NUM,

  .xclk_freq_hz = 10000000,
  .ledc_timer = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,

  .pixel_format = PIXFORMAT_JPEG,
  .frame_size = FRAMESIZE_QVGA,
  .jpeg_quality = 12,

  .fb_count = 1,
  .fb_location = CAMERA_FB_IN_PSRAM,
  .grab_mode = CAMERA_GRAB_LATEST,
};

// ===============================
// PROTOTYPES
// ===============================
bool initCamera();
bool captureForModel(uint32_t img_width, uint32_t img_height, uint8_t *out_buf);
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr);

void addCors();
void handleOptions();
void handleRoot();
void handleCapture();
void handleStatus();
void handleScan();
void handleReset();

void findClassIndexes();

bool runOnePrediction(float &authorizedScore, float &notAuthorizedScore);
void applyDecision(float authorizedScore, float notAuthorizedScore);

void openDoorVirtual();
void closeDoorVirtual();
void updateDoorTimeout();

void sendToThingSpeak();
String buildStatusJson(bool okValue);

// ===============================
// CORS
// ===============================
void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-store");
}

void handleOptions() {
  addCors();
  server.send(204, "text/plain", "");
}

// ===============================
// LABELS
// ===============================
void findClassIndexes() {
  authorizedIndex = -1;
  notAuthorizedIndex = -1;

  for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    String label = String(ei_classifier_inferencing_categories[i]);
    label.toLowerCase();
    label.trim();

    if (label == "authorized" || label == "autorise") {
      authorizedIndex = i;
    }

    if (
      label == "not_authorized" ||
      label == "not authorized" ||
      label == "inconnu" ||
      label == "unknown"
    ) {
      notAuthorizedIndex = i;
    }
  }
}

// ===============================
// HTTP HANDLERS
// ===============================
void handleRoot() {
  addCors();
  server.send(200, "text/plain", "FaceDoor ESP32 API. Use /capture /status /scan /reset");
}

void handleCapture() {
  if (!cameraReady || isBusy) {
    addCors();
    server.send(503, "text/plain", "Camera busy or not ready");
    return;
  }

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    addCors();
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  dbgFrameWidth = fb->width;
  dbgFrameHeight = fb->height;
  dbgFrameFormat = fb->format;
  dbgFrameLength = fb->len;

  WiFiClient client = server.client();

  client.println("HTTP/1.1 200 OK");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Cache-Control: no-store");
  client.println("Content-Type: image/jpeg");
  client.print("Content-Length: ");
  client.println(fb->len);
  client.println("Connection: close");
  client.println();

  client.write(fb->buf, fb->len);

  esp_camera_fb_return(fb);

  delay(20);
  client.stop();
}

String buildStatusJson(bool okValue) {
  float confidencePercent = 0.0f;

  if (lastDecision == "AUTHORIZED") {
    confidencePercent = lastAuthorizedScore * 100.0f;
  } else if (lastDecision == "NOT_AUTHORIZED") {
    confidencePercent = lastNotAuthorizedScore * 100.0f;
  }

  String json = "{";

  json += "\"ok\":" + String(okValue ? "true" : "false") + ",";
  json += "\"busy\":" + String(isBusy ? "true" : "false") + ",";
  json += "\"eventId\":" + String(eventId) + ",";
  json += "\"decision\":\"" + lastDecision + "\",";
  json += "\"door\":\"" + doorState + "\",";
  json += "\"authorized\":" + String(lastAuthorizedScore, 4) + ",";
  json += "\"not_authorized\":" + String(lastNotAuthorizedScore, 4) + ",";
  json += "\"threshold\":" + String(AUTH_THRESHOLD, 2) + ",";
  json += "\"model\":\"" + modelMode + "\",";
  json += "\"total_authorized\":" + String(totalAuthorized) + ",";
  json += "\"total_refused\":" + String(totalRefused) + ",";
  json += "\"authorized_hits\":" + String(authorizedHits) + ",";
  json += "\"refused_hits\":" + String(refusedHits) + ",";
  json += "\"required_hits\":" + String(REQUIRED_AUTHORIZED_HITS) + ",";
  json += "\"confidence_percent\":" + String(confidencePercent, 1) + ",";
  json += "\"frame_width\":" + String(dbgFrameWidth) + ",";
  json += "\"frame_height\":" + String(dbgFrameHeight) + ",";
  json += "\"frame_format\":" + String(dbgFrameFormat) + ",";
  json += "\"frame_length\":" + String(dbgFrameLength) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"free_psram\":" + String(ESP.getFreePsram());

  json += "}";

  return json;
}

void handleStatus() {
  addCors();

  String json = buildStatusJson(cameraReady);

  server.send(200, "application/json", json);
}

void handleReset() {
  addCors();

  eventId = 0;
  totalAuthorized = 0;
  totalRefused = 0;

  authorizedHits = 0;
  refusedHits = 0;

  lastAuthorizedScore = 0.0f;
  lastNotAuthorizedScore = 0.0f;

  lastDecision = "WAITING";
  closeDoorVirtual();

  String json = buildStatusJson(true);

  server.send(200, "application/json", json);

  Serial.println("Reset OK");
}

void handleScan() {
  addCors();

  if (!cameraReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"camera_not_ready\"}");
    return;
  }

  if (isBusy) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }

  isBusy = true;

  float auth = 0.0f;
  float notAuth = 0.0f;

  bool ok = runOnePrediction(auth, notAuth);

  if (ok) {
    applyDecision(auth, notAuth);
  } else {
    lastDecision = "NOT_AUTHORIZED";
    lastAuthorizedScore = 0.0f;
    lastNotAuthorizedScore = 1.0f;

    totalRefused++;
    eventId++;

    authorizedHits = 0;
    refusedHits++;

    closeDoorVirtual();

    Serial.println("SCAN: ERROR -> NOT_AUTHORIZED");
  }

  isBusy = false;

  sendToThingSpeak();

  String json = buildStatusJson(ok);

  server.send(200, "application/json", json);
}

// ===============================
// SETUP
// ===============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!psramFound()) {
    Serial.println("ERROR: PSRAM");
    while (true) delay(1000);
  }

  if (!initCamera()) {
    Serial.println("ERROR: CAMERA");
    while (true) delay(1000);
  }

  cameraReady = true;

  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();

    if (fb) {
      esp_camera_fb_return(fb);
    }

    delay(200);
  }

  findClassIndexes();

  if (authorizedIndex == -1 || notAuthorizedIndex == -1) {
    Serial.println("ERROR: LABELS");
    while (true) delay(1000);
  }

  closeDoorVirtual();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print("WiFi");

  int tries = 0;

  while (WiFi.status() != WL_CONNECTED && tries < 50) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("ERROR: WIFI");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/reset", HTTP_GET, handleReset);

  server.on("/", HTTP_OPTIONS, handleOptions);
  server.on("/capture", HTTP_OPTIONS, handleOptions);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/scan", HTTP_OPTIONS, handleOptions);
  server.on("/reset", HTTP_OPTIONS, handleOptions);

  server.begin();

  Serial.println("READY");
}

// ===============================
// LOOP
// ===============================
void loop() {
  server.handleClient();
  updateDoorTimeout();
  delay(5);
}

// ===============================
// CAMERA INIT
// ===============================
bool initCamera() {
  if (is_initialised) return true;

  esp_err_t err = esp_camera_init(&camera_config);

  if (err != ESP_OK) {
    is_initialised = false;
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (s != NULL) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);

    s->set_gain_ctrl(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_aec2(s, 1);

    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
  }

  is_initialised = true;

  return true;
}

// ===============================
// INFERENCE
// ===============================
bool runOnePrediction(float &authorizedScore, float &notAuthorizedScore) {
  size_t snapshot_size =
    EI_CAMERA_RAW_FRAME_BUFFER_COLS *
    EI_CAMERA_RAW_FRAME_BUFFER_ROWS *
    EI_CAMERA_FRAME_BYTE_SIZE;

  snapshot_buf = (uint8_t*)ps_malloc(snapshot_size);

  if (snapshot_buf == nullptr) {
    return false;
  }

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &ei_camera_get_data;

  bool captured = captureForModel(
    (uint32_t)EI_CLASSIFIER_INPUT_WIDTH,
    (uint32_t)EI_CLASSIFIER_INPUT_HEIGHT,
    snapshot_buf
  );

  if (!captured) {
    free(snapshot_buf);
    snapshot_buf = nullptr;
    Serial.println("SCAN: CAPTURE_FAILED");
    return false;
  }

  ei_impulse_result_t result = { 0 };

  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);

  if (err != EI_IMPULSE_OK) {
    free(snapshot_buf);
    snapshot_buf = nullptr;
    Serial.println("SCAN: MODEL_FAILED");
    return false;
  }

  authorizedScore = result.classification[authorizedIndex].value;
  notAuthorizedScore = result.classification[notAuthorizedIndex].value;

  free(snapshot_buf);
  snapshot_buf = nullptr;

  return true;
}

bool captureForModel(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
  if (!cameraReady) {
    return false;
  }

  camera_fb_t *fb = esp_camera_fb_get();

  if (!fb) {
    return false;
  }

  dbgFrameWidth = fb->width;
  dbgFrameHeight = fb->height;
  dbgFrameFormat = fb->format;
  dbgFrameLength = fb->len;

  bool converted = fmt2rgb888(
    fb->buf,
    fb->len,
    PIXFORMAT_JPEG,
    snapshot_buf
  );

  esp_camera_fb_return(fb);

  if (!converted) {
    return false;
  }

  if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS) ||
      (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
    ei::image::processing::crop_and_interpolate_rgb888(
      out_buf,
      EI_CAMERA_RAW_FRAME_BUFFER_COLS,
      EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
      out_buf,
      img_width,
      img_height
    );
  }

  return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t pixel_ix = offset * 3;
  size_t pixels_left = length;
  size_t out_ptr_ix = 0;

  while (pixels_left != 0) {
    out_ptr[out_ptr_ix] =
      (snapshot_buf[pixel_ix] << 16) +
      (snapshot_buf[pixel_ix + 1] << 8) +
      snapshot_buf[pixel_ix + 2];

    out_ptr_ix++;
    pixel_ix += 3;
    pixels_left--;
  }

  return 0;
}

// ===============================
// DECISION
// ===============================
void applyDecision(float authorizedScore, float notAuthorizedScore) {
  lastAuthorizedScore = authorizedScore;
  lastNotAuthorizedScore = notAuthorizedScore;

  eventId++;

  bool currentAuthorized =
    authorizedScore >= AUTH_THRESHOLD &&
    authorizedScore > notAuthorizedScore;

  if (currentAuthorized) {
    lastDecision = "AUTHORIZED";
    totalAuthorized++;

    authorizedHits++;
    refusedHits = 0;

    if (authorizedHits >= REQUIRED_AUTHORIZED_HITS) {
      openDoorVirtual();

      Serial.println("DECISION: OPEN");
    } else {
      doorState = "CLOSED";

      Serial.println("DECISION: AUTH_HIT");
    }
  } else {
    lastDecision = "NOT_AUTHORIZED";
    totalRefused++;

    refusedHits++;
    authorizedHits = 0;

    closeDoorVirtual();

    Serial.println("DECISION: CLOSED");
  }
}

void openDoorVirtual() {
  doorState = "OPEN";
  doorOpenedAt = millis();
}

void closeDoorVirtual() {
  doorState = "CLOSED";
}

void updateDoorTimeout() {
  if (doorState == "OPEN" && millis() - doorOpenedAt > DOOR_OPEN_MS) {
    closeDoorVirtual();

    authorizedHits = 0;
    refusedHits = 0;

    Serial.println("DECISION: AUTO_CLOSE");
  }
}

// ===============================
// THINGSPEAK
// ===============================
void sendToThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();

  if (now - lastThingSpeakSend < THINGSPEAK_MIN_INTERVAL_MS) {
    return;
  }

  lastThingSpeakSend = now;

  int accessValue = lastDecision == "AUTHORIZED" ? 1 : 0;
  int doorValue = doorState == "OPEN" ? 1 : 0;
  int decisionCode = lastDecision == "AUTHORIZED" ? 1 : 0;

  float confidencePercent = 0.0f;

  if (lastDecision == "AUTHORIZED") {
    confidencePercent = lastAuthorizedScore * 100.0f;
  } else {
    confidencePercent = lastNotAuthorizedScore * 100.0f;
  }

  HTTPClient http;

  String url = String(THINGSPEAK_URL);
  url += "?api_key=" + String(THINGSPEAK_API_KEY);
  url += "&field1=" + String(accessValue);
  url += "&field2=" + String(lastAuthorizedScore * 100.0f, 1);
  url += "&field3=" + String(lastNotAuthorizedScore * 100.0f, 1);
  url += "&field4=" + String(doorValue);
  url += "&field5=" + String(authorizedHits);
  url += "&field6=" + String(refusedHits);

  http.begin(url);

  int httpCode = http.GET();

  Serial.print("TS: ");
  Serial.println(httpCode);

  http.end();
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif