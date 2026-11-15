#ifndef ESPALTHERMA_REPRO663_H
#define ESPALTHERMA_REPRO663_H

// Diagnosis of #663 only, never part of a release. Three things a bare ESP32-S3 lacks to show what an
// M5StickC Plus does during a web update:
//  - a heat pump: fakeUnitReply() answers as a unit of FAKE_UNIT_MODEL would (repro663_unit.h carries the bytes);
//  - as little memory: /api/repro/ballast?free=N holds heap back until N bytes of data heap are left;
//  - a way to read a crash without a serial cable: the allocations that failed are kept in RTC memory, and
//    /api/repro/coredump hands out the core dump the panic handler wrote to flash.

#include <Arduino.h>
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
#define FAILED_ALLOCATIONS_KEPT 8

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

// ------------------------------------------------------------------------------------------------ the routes

static void reproHeapJson(JsonDocument &doc)
{
  doc["free"] = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  doc["lowest"] = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  doc["block"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  doc["total"] = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  doc["ballast"] = ballastPieces.size() * BALLAST_PIECE_BYTES;
  doc["uptime_ms"] = millis();
  failedAllocationsToJson(failedAllocations, doc["failed"].to<JsonObject>());
  failedAllocationsToJson(failedAllocationsOfPreviousBoot, doc["failed_previous_boot"].to<JsonObject>());
}

static void reproSendHeap(AsyncWebServerRequest *request)
{
  JsonDocument doc;
  reproHeapJson(doc);
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
}

#endif
