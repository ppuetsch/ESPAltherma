#ifndef ESPALTHERMA_REPRO663_H
#define ESPALTHERMA_REPRO663_H

// Diagnosis of #663 only, never part of a release. Three things a bare ESP32-S3 lacks to show what an
// M5StickC Plus does during a web update:
//  - a heat pump: fakeUnitReply() answers as a unit of FAKE_UNIT_MODEL would (repro663_unit.h carries the bytes);
//  - as little memory: /api/repro/ballast?free=N holds heap back until N bytes of data heap are left;
//  - a way to read a crash without a serial cable: the allocations that failed are kept in RTC memory, and
//    /api/repro/coredump hands out the core dump the panic handler wrote to flash.

#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_attr.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <vector>
#include "catalog.h"
#include "detect.h"

#define FAKE_UNIT_MODEL "Altherma(EPRA E ETV16-ETB16-ETVZ16 E_EJ series 8-12kW)"
#define FAKE_UNIT_REGISTRIES 256
#define FAKE_UNIT_PAYLOAD_MAX 60
#define BALLAST_PIECE_BYTES 1024
#define FAILED_ALLOCATION_MAGIC 0x4F4F4D21u // "OOM!"
#define FAILED_ALLOCATIONS_KEPT 24

// ---------------------------------------------------------------------------------------------- the heat pump

struct FakeUnitRegistry
{
  bool answers;
  uint8_t payloadLength;
  uint8_t payload[FAKE_UNIT_PAYLOAD_MAX];
};

static FakeUnitRegistry *fakeUnitRegistries = nullptr;

// What a healthy unit reads for a value of this kind, in the unit of its conversion (-1: left at zero)
static int fakeUnitPlausibleRaw(const CatalogEntry &entry)
{
  switch (detectQuantity(entry))
  {
  case QTY_TEMP:
    return 200;
  case QTY_VOLTAGE:
    return entry.conv == 105 ? 2300 : 230;
  case QTY_CURRENT:
    return 50;
  case QTY_FLOW:
    return 150;
  default:
    return -1;
  }
}

static void fakeUnitStoreRaw(const CatalogEntry &entry, int raw, uint8_t *at)
{
  bool bigEndian = entry.conv == 152 || entry.conv == 161;
  if (entry.size == 1)
    at[0] = raw & 0xff;
  else if (bigEndian)
  {
    at[0] = raw >> 8;
    at[1] = raw & 0xff;
  }
  else
  {
    at[0] = raw & 0xff;
    at[1] = raw >> 8;
  }
}

// The same unit test/test_detect builds for a model: each of its registries as long as the model reads it,
// with plausible temperatures, voltages, currents and flow.
static void fakeUnitBuild()
{
  fakeUnitRegistries = (FakeUnitRegistry *)calloc(FAKE_UNIT_REGISTRIES, sizeof(FakeUnitRegistry));
  int model = catalogFindModel(FAKE_UNIT_MODEL);
  if (fakeUnitRegistries == nullptr || model < 0)
    return;
  for (int i = 0; i < CATALOG_ENTRY_COUNT; i++)
  {
    const CatalogEntry &entry = CATALOG_ENTRIES[i];
    bool fits = entry.size > 0 && entry.offset + entry.size <= FAKE_UNIT_PAYLOAD_MAX;
    if (!catalogInModel(entry, model) || !fits)
      continue;
    FakeUnitRegistry &registry = fakeUnitRegistries[entry.reg];
    registry.answers = true;
    if (entry.offset + entry.size > registry.payloadLength)
      registry.payloadLength = entry.offset + entry.size;
    int raw = fakeUnitPlausibleRaw(entry);
    if (raw >= 0)
      fakeUnitStoreRaw(entry, raw, registry.payload + entry.offset);
  }
  const CatalogModel &catalogModel = CATALOG_MODELS[model];
  FakeUnitRegistry &outdoor = fakeUnitRegistries[0x00];
  if (outdoor.answers && outdoor.payloadLength > 12)
    outdoor.payload[12] = (catalogModel.capMin + catalogModel.capMax) * 5; // outdoor capacity, kW x 10
}

static uint8_t fakeUnitChecksum(const uint8_t *frame, size_t length)
{
  uint8_t sum = 0;
  for (size_t i = 0; i < length; i++)
    sum += frame[i];
  return (uint8_t)~sum;
}

static size_t fakeUnitNotUnderstood(uint8_t *reply)
{
  reply[0] = 0x15;
  reply[1] = 0xEA;
  return 2;
}

// A registry query (03 40 <registry> <checksum>) gets 40 <registry> <length> <payload> <checksum>; everything
// else, the memory commands of the controls included, gets the "not understood" of a board without them.
size_t fakeUnitReply(const uint8_t *query, size_t queryLength, uint8_t *reply)
{
  if (fakeUnitRegistries == nullptr)
    fakeUnitBuild();
  bool registryQuery = queryLength == 4 && query[0] == 0x03 && query[1] == 0x40;
  if (!registryQuery || fakeUnitRegistries == nullptr || !fakeUnitRegistries[query[2]].answers)
    return fakeUnitNotUnderstood(reply);
  const FakeUnitRegistry &registry = fakeUnitRegistries[query[2]];
  reply[0] = 0x40;
  reply[1] = query[2];
  reply[2] = registry.payloadLength + 2;
  memcpy(reply + 3, registry.payload, registry.payloadLength);
  size_t withoutChecksum = 3 + registry.payloadLength;
  reply[withoutChecksum] = fakeUnitChecksum(reply, withoutChecksum);
  return withoutChecksum + 1;
}

// ------------------------------------------------------------------------------------- the memory held back

static std::vector<void *> ballastPieces;

static void ballastRelease()
{
  for (void *piece : ballastPieces)
    free(piece);
  ballastPieces.clear();
}

static void ballastHoldUntilFree(uint32_t wantedFree)
{
  ballastRelease();
  ballastPieces.reserve(512);
  while (heap_caps_get_free_size(MALLOC_CAP_8BIT) > wantedFree + BALLAST_PIECE_BYTES && ballastPieces.size() < ballastPieces.capacity())
  {
    void *piece = heap_caps_malloc(BALLAST_PIECE_BYTES, MALLOC_CAP_8BIT);
    if (piece == nullptr)
      break;
    ballastPieces.push_back(piece);
  }
}

// ------------------------------------------------------------------------- where the memory is at its lowest

// The heap's own low-water mark cannot be set back and does not say who took the memory: a task samples the free
// data heap every 2 ms and keeps the lowest figure with the step the main loop was in (stage.h), until asked to
// start again (/api/repro/heap?restart=1).
#define HEAP_SAMPLE_EVERY_MS 2
extern volatile const char *loopStage;
static volatile uint32_t sampledLowestFree = UINT32_MAX, sampledLowestBlock = 0, sampledLowestAtMs = 0;
static volatile const char *sampledLowestStage = "";

static void heapSamplerRestart()
{
  sampledLowestFree = UINT32_MAX;
}

static void heapSamplerTask(void *)
{
  for (;;)
  {
    uint32_t freeNow = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeNow < sampledLowestFree)
    {
      sampledLowestFree = freeNow;
      sampledLowestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      sampledLowestStage = loopStage;
      sampledLowestAtMs = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(HEAP_SAMPLE_EVERY_MS));
  }
}

static void heapSamplerBegin()
{
  xTaskCreate(heapSamplerTask, "heapsampler", 3072, nullptr, configMAX_PRIORITIES - 2, nullptr);
}

// ----------------------------------------------------------------------- the allocations that found no room

struct FailedAllocation
{
  uint32_t uptimeMs;
  uint32_t bytes;
  uint32_t freeBytes;
  uint32_t largestBlock;
  char task[16];
};

struct FailedAllocationLog
{
  uint32_t magic;
  uint32_t total;
  FailedAllocation last[FAILED_ALLOCATIONS_KEPT];
};

RTC_NOINIT_ATTR FailedAllocationLog failedAllocations;
static FailedAllocationLog failedAllocationsOfPreviousBoot;

static void failedAllocationNoted(size_t bytes, uint32_t, const char *)
{
  FailedAllocation &entry = failedAllocations.last[failedAllocations.total % FAILED_ALLOCATIONS_KEPT];
  entry.uptimeMs = millis();
  entry.bytes = bytes;
  entry.freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  entry.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const char *task = xPortInIsrContext() ? "interrupt" : pcTaskGetName(nullptr);
  strlcpy(entry.task, task != nullptr ? task : "?", sizeof(entry.task));
  failedAllocations.total++;
}

// Before anything else at start: what the previous boot left is set aside, and this boot starts an empty list
void reproBegin()
{
  bool kept = failedAllocations.magic == FAILED_ALLOCATION_MAGIC;
  if (kept)
    failedAllocationsOfPreviousBoot = failedAllocations;
  memset(&failedAllocations, 0, sizeof(failedAllocations));
  failedAllocations.magic = FAILED_ALLOCATION_MAGIC;
  heap_caps_register_failed_alloc_callback(failedAllocationNoted);
}

static void failedAllocationsToJson(const FailedAllocationLog &log, JsonObject out)
{
  out["total"] = log.total;
  JsonArray last = out["last"].to<JsonArray>();
  uint32_t kept = log.total < FAILED_ALLOCATIONS_KEPT ? log.total : FAILED_ALLOCATIONS_KEPT;
  for (uint32_t i = 0; i < kept; i++)
  {
    const FailedAllocation &entry = log.last[(log.total - kept + i) % FAILED_ALLOCATIONS_KEPT];
    JsonObject item = last.add<JsonObject>();
    item["uptime_ms"] = entry.uptimeMs;
    item["bytes"] = entry.bytes;
    item["free"] = entry.freeBytes;
    item["block"] = entry.largestBlock;
    item["task"] = entry.task;
  }
}

// ------------------------------------------------------------------------------------- the broker next door

// The second bare board takes what the first one publishes: enough of MQTT 3.1.1 for PubSubClient (CONNECT,
// SUBSCRIBE, PINGREQ, QoS 0 PUBLISH). Nothing is forwarded; the bytes are counted for /api/repro/broker.
#define BROKER_PORT 1883
#define MQTT_PACKET_CONNECT 1
#define MQTT_PACKET_PUBLISH 3
#define MQTT_PACKET_SUBSCRIBE 8
#define MQTT_PACKET_PINGREQ 12

struct BrokerConnection
{
  enum { READ_TYPE, READ_LENGTH, READ_BODY } state = READ_TYPE;
  uint8_t packetType = 0;
  uint32_t bodyLength = 0, bodyRead = 0;
  uint8_t lengthShift = 0;
  uint8_t packetId[2] = {0, 0};
};

static AsyncServer brokerServer(BROKER_PORT);
static volatile uint32_t brokerConnections = 0, brokerPublishes = 0, brokerPublishedBytes = 0, brokerLargestPublish = 0;

static void brokerAnswer(AsyncClient *board, const BrokerConnection &connection)
{
  static const char connack[] = {0x20, 0x02, 0x00, 0x00};
  static const char pingresp[] = {(char)0xD0, 0x00};
  if (connection.packetType == MQTT_PACKET_CONNECT)
    board->write(connack, sizeof(connack));
  else if (connection.packetType == MQTT_PACKET_PINGREQ)
    board->write(pingresp, sizeof(pingresp));
  else if (connection.packetType == MQTT_PACKET_SUBSCRIBE)
  {
    const char suback[] = {(char)0x90, 0x03, (char)connection.packetId[0], (char)connection.packetId[1], 0x00};
    board->write(suback, sizeof(suback));
  }
  else if (connection.packetType == MQTT_PACKET_PUBLISH)
  {
    brokerPublishes++;
    brokerPublishedBytes += connection.bodyLength;
    if (connection.bodyLength > brokerLargestPublish)
      brokerLargestPublish = connection.bodyLength;
  }
}

static void brokerPacketEnded(AsyncClient *board, BrokerConnection &connection)
{
  brokerAnswer(board, connection);
  connection.state = BrokerConnection::READ_TYPE;
}

static void brokerTakeByte(AsyncClient *board, BrokerConnection &connection, uint8_t byte)
{
  switch (connection.state)
  {
  case BrokerConnection::READ_TYPE:
    connection.packetType = byte >> 4;
    connection.bodyLength = connection.bodyRead = 0;
    connection.lengthShift = 0;
    connection.state = BrokerConnection::READ_LENGTH;
    break;
  case BrokerConnection::READ_LENGTH:
    connection.bodyLength |= (uint32_t)(byte & 0x7F) << connection.lengthShift;
    connection.lengthShift += 7;
    if (byte & 0x80)
      break;
    connection.state = BrokerConnection::READ_BODY;
    if (connection.bodyLength == 0)
      brokerPacketEnded(board, connection);
    break;
  case BrokerConnection::READ_BODY:
    if (connection.bodyRead < sizeof(connection.packetId))
      connection.packetId[connection.bodyRead] = byte;
    if (++connection.bodyRead == connection.bodyLength)
      brokerPacketEnded(board, connection);
    break;
  }
}

static void brokerBegin()
{
  brokerServer.onClient([](void *, AsyncClient *board) {
    BrokerConnection *connection = new BrokerConnection();
    brokerConnections++;
    board->onData([connection](void *, AsyncClient *from, void *data, size_t length) {
      for (size_t i = 0; i < length; i++)
        brokerTakeByte(from, *connection, ((const uint8_t *)data)[i]);
    });
    board->onDisconnect([connection](void *, AsyncClient *gone) {
      delete connection;
      delete gone;
    });
  }, nullptr);
  brokerServer.begin();
}

static void brokerSendCounts(AsyncWebServerRequest *request)
{
  JsonDocument doc;
  doc["connections"] = brokerConnections;
  doc["publishes"] = brokerPublishes;
  doc["published_bytes"] = brokerPublishedBytes;
  doc["largest_publish"] = brokerLargestPublish;
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

// ------------------------------------------------------------------------------------------------ the routes

static void reproHeapJson(JsonDocument &doc)
{
  doc["free"] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  doc["lowest"] = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  doc["block"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  doc["total"] = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  doc["ballast"] = ballastPieces.size() * BALLAST_PIECE_BYTES;
  doc["uptime_ms"] = millis();
  JsonObject sampled = doc["sampled"].to<JsonObject>();
  sampled["lowest"] = sampledLowestFree;
  sampled["block"] = sampledLowestBlock;
  sampled["stage"] = (const char *)sampledLowestStage;
  sampled["at_ms"] = sampledLowestAtMs;
  failedAllocationsToJson(failedAllocations, doc["failed"].to<JsonObject>());
  failedAllocationsToJson(failedAllocationsOfPreviousBoot, doc["failed_previous_boot"].to<JsonObject>());
}

static void reproSendHeap(AsyncWebServerRequest *request)
{
  JsonDocument doc;
  reproHeapJson(doc);
  if (request->hasParam("restart"))
    heapSamplerRestart(); // after the figures are taken: the answer itself costs memory
  String body;
  serializeJson(doc, body);
  request->send(200, "application/json", body);
}

static void reproSendCoreDump(AsyncWebServerRequest *request)
{
  size_t flashAddress = 0, imageSize = 0;
  const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  bool found = partition != nullptr && esp_core_dump_image_get(&flashAddress, &imageSize) == ESP_OK;
  if (!found)
  {
    request->send(404, "text/plain", "no core dump");
    return;
  }
  size_t offsetInPartition = flashAddress - partition->address;
  request->send(request->beginChunkedResponse("application/octet-stream", [partition, offsetInPartition, imageSize](uint8_t *out, size_t room, size_t index) -> size_t {
    if (index >= imageSize)
      return 0;
    size_t length = imageSize - index < room ? imageSize - index : room;
    return esp_partition_read(partition, offsetInPartition + index, out, length) == ESP_OK ? length : 0;
  }));
}

void reproRoutes(AsyncWebServer &server)
{
  server.on("/api/repro/heap", HTTP_GET, reproSendHeap);
  server.on("/api/repro/ballast", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("free"))
      ballastHoldUntilFree(request->getParam("free")->value().toInt());
    else
      ballastRelease();
    reproSendHeap(request);
  });
  server.on("/api/repro/coredump/erase", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", esp_core_dump_image_erase() == ESP_OK ? "erased" : "nothing erased");
  });
  server.on("/api/repro/coredump", HTTP_GET, reproSendCoreDump);
  server.on("/api/repro/broker", HTTP_GET, brokerSendCounts);
  brokerBegin();
  heapSamplerBegin();
}

#endif
