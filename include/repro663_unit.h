#ifndef ESPALTHERMA_REPRO663_UNIT_H
#define ESPALTHERMA_REPRO663_UNIT_H

// Diagnosis of #663 only, never part of a release: a heat pump that lives in the firmware, for a board with
// nothing on its X10A pins. It takes the place of the serial port (comm.h MySerial), so the polling, the survey
// and the controls run their own code unchanged: a query written to it is answered at the pace of a 9600 baud
// 8E1 line. The replies themselves come from fakeUnitReply() (repro663.h), which knows the catalog.

#include <Arduino.h>

#define FAKE_UNIT_REPLY_MAX 80
#define FAKE_UNIT_LATENCY_MS 20           // what a hydro board takes before its first byte
#define FAKE_UNIT_MICROS_PER_BYTE 1146UL  // 11 bits at 9600 baud

size_t fakeUnitReply(const uint8_t *query, size_t queryLength, uint8_t *reply);

class FakeUnitSerial
{
public:
  void begin(unsigned long, uint32_t, int8_t, int8_t) {}
  void flush(bool = true) {}

  size_t write(const uint8_t *query, size_t queryLength)
  {
    replyLength = fakeUnitReply(query, queryLength, reply);
    replyRead = 0;
    replyStartMicros = micros() + queryLength * FAKE_UNIT_MICROS_PER_BYTE + FAKE_UNIT_LATENCY_MS * 1000UL;
    return queryLength;
  }

  int available()
  {
    long sinceStart = (long)(micros() - replyStartMicros);
    if (sinceStart < 0)
      return 0;
    size_t arrived = 1 + (size_t)sinceStart / FAKE_UNIT_MICROS_PER_BYTE;
    if (arrived > replyLength)
      arrived = replyLength;
    return (int)(arrived - replyRead);
  }

  int read()
  {
    return available() > 0 ? reply[replyRead++] : -1;
  }

private:
  uint8_t reply[FAKE_UNIT_REPLY_MAX];
  size_t replyLength = 0;
  size_t replyRead = 0;
  unsigned long replyStartMicros = 0;
};

#endif
