#ifndef ESPALTHERMA_OUTPUTS_H
#define ESPALTHERMA_OUTPUTS_H

// Experimental: forces the three-way valve (space heating or hot water) and the space heating pump of the hydro
// board through its X10A port, for a heat source the unit does not know about (a stove, a solar loop) or a test.
//
// The hydro board software 20002203 answers a service command 'B' that overrides its outputs: 13 data bytes, one
// per output, 0xFF leaving an output to the unit, any other value forcing it. Byte 5 is the three-way valve (1 hot
// water, 0 space heating). The pump needs two things: byte 0, its run request, and bytes 7-8, the PWM word of its
// speed signal (high byte first; 0 is full speed, 0xFFFF stop). A run request with the signal at "stop" does not
// turn it. The command 'S' reads the outputs back: the forced value where one is forced, else the board's own.
// Found in the board's software and checked on an EHBX08EF6V (November 2026): the valve moved to hot water and
// back, the pump ran at 9.5 l/min with a 45 % signal (the unit's own actuator test uses the same), with space
// heating and hot water switched off on the interface.
//
// The board keeps a forced output until it restarts or is told otherwise: nothing on its side ends it. So here:
// - every forced output ends by itself OUTPUT_HOLD_MS after the last request (an automation that wants it longer
//   sends its request again),
// - the outputs are released once when ESPAltherma starts (one forced before a restart of ESPAltherma ends there),
//   and before the option is switched off,
// - what the board reports is read back every OUTPUT_CHECK_MS, and a forced output it lost (the unit restarted) is
//   forced again.
// Only the software in OUTPUT_PROFILES is supported; nothing is sent to another one.
//
// No Arduino dependency: the frame exchange is injected (control.h), so a unit test drives it with a fake unit.

#include "control.h"

#define OUTPUT_COMMAND_FORCE 0x42 // 'B'
#define OUTPUT_COMMAND_READ 0x53  // 'S'
#define OUTPUT_FORCE_BYTES 13
#define OUTPUT_READ_BYTES 16
#define OUTPUT_NOT_FORCED 0xFF
#define OUTPUT_HOLD_MS 3600000UL // a forced output ends an hour after the last request
#define OUTPUT_CHECK_MS 60000UL  // the outputs are read back this often
#define OUTPUT_PUMP_SPEED_MIN 10 // % of full speed; the unit's own actuator test runs the pump at 55 %
#define OUTPUT_PUMP_SPEED_MAX 100
#define OUTPUT_PWM_FULL_SCALE 65535L

// Where a hydro board software keeps its outputs in the 'B' and 'S' commands
struct OutputProfile
{
  const char *softwareId;
  uint8_t valveByte;    // 'B' and 'S': the three-way valve
  uint8_t pumpByte;     // 'B' and 'S': the pump's run request
  uint8_t pwmHighByte;  // 'B': the pump signal's PWM word, high byte
  uint8_t pwmLowByte;   // and low byte
};

static const OutputProfile OUTPUT_PROFILES[] = {
    // 4-8 kW EHVH/EHVX/EHVZ/EHBH/EHBX E series, 9-16 kW EBLA/EDLA monoblocs: handlers at 0x0A94DE ('B') and
    // 0x0A964A ('S'), the valve output at 0x0B4782, the pump's request and signal at 0x0B1FAC and 0x0B1F9E
    {"20002203", 5, 0, 7, 8},
};
#define OUTPUT_PROFILE_COUNT (sizeof(OUTPUT_PROFILES) / sizeof(OUTPUT_PROFILES[0]))

enum ValveForce : int8_t
{
  VALVE_AUTO = -1, // the unit moves it
  VALVE_SPACE = 0,
  VALVE_DHW = 1
};

#define OUTPUT_NO_REQUEST -2 // wantedValve / wantedPump: nothing asked

inline const char *valveForceName(int v)
{
  return v == VALVE_DHW ? "dhw" : v == VALVE_SPACE ? "space" : "auto";
}

// "auto", "space" or "dhw" to a ValveForce; OUTPUT_NO_REQUEST for anything else
inline int valveForceParse(const char *s)
{
  if (strcmp(s, "auto") == 0)
    return VALVE_AUTO;
  if (strcmp(s, "space") == 0)
    return VALVE_SPACE;
  if (strcmp(s, "dhw") == 0)
    return VALVE_DHW;
  return OUTPUT_NO_REQUEST;
}

// The pump signal for a speed in % of full speed: the PWM word counts the time at "slow", full speed is 0
inline uint16_t outputPumpPwm(int speed)
{
  if (speed < OUTPUT_PUMP_SPEED_MIN)
    speed = OUTPUT_PUMP_SPEED_MIN;
  if (speed > OUTPUT_PUMP_SPEED_MAX)
    speed = OUTPUT_PUMP_SPEED_MAX;
  return (uint16_t)(((OUTPUT_PUMP_SPEED_MAX - speed) * OUTPUT_PWM_FULL_SCALE) / 100);
}

struct OutputControl : X10aClient
{
  const OutputProfile *profile = nullptr;
  ControlStatus status = CONTROL_OFF;
  char softwareId[9] = "";
  int valve = VALVE_AUTO;      // forced from here
  int pumpSpeed = 0;           // forced from here, % of full speed (0: the pump is left to the unit)
  int reportedValve = -1;      // what the board reports (0 space heating, 1 hot water, -1 unknown)
  int reportedPump = -1;       // its pump run request (0, 1, -1 unknown)
  volatile int wantedValve = OUTPUT_NO_REQUEST; // set from other tasks
  volatile int wantedPump = OUTPUT_NO_REQUEST;  // speed, 0 to leave the pump to the unit
  volatile bool changed = false;
  bool cleared = false;        // the outputs were released once since ESPAltherma started
  bool expired = false;        // a forced output ended by itself since the last read, for the caller's log
  unsigned long forcedAt = 0, readAt = 0, checkedAt = 0;
  unsigned long retryAfter = CONTROL_RETRY_FIRST_MS;
  bool identificationTried = false;

  bool forcing() const { return valve != VALVE_AUTO || pumpSpeed > 0; }

  void reset()
  {
    profile = nullptr;
    status = CONTROL_OFF;
    softwareId[0] = 0;
    reportedValve = reportedPump = -1;
    wantedValve = wantedPump = OUTPUT_NO_REQUEST;
    retryAfter = CONTROL_RETRY_FIRST_MS;
    identificationTried = false;
    changed = true;
  }

  // Reads the software version and checks the profile. True when ready.
  bool identify(unsigned long now)
  {
    bool retrying = identificationTried && status != CONTROL_READY;
    identificationTried = true;
    checkedAt = now;
    ControlStatus before = status;
    if (!readSoftwareId(softwareId))
      status = CONTROL_NO_ANSWER;
    else
    {
      profile = nullptr;
      for (size_t i = 0; i < OUTPUT_PROFILE_COUNT; i++)
        if (strcmp(OUTPUT_PROFILES[i].softwareId, softwareId) == 0)
          profile = &OUTPUT_PROFILES[i];
      status = profile == nullptr ? CONTROL_UNSUPPORTED : readBack(now) ? CONTROL_READY : CONTROL_NO_ANSWER;
    }
    if (status != CONTROL_READY)
      reportedValve = reportedPump = -1;
    if (status != before)
      changed = true;
    retryAfter = status == CONTROL_READY || !retrying ? CONTROL_RETRY_FIRST_MS : CurveControl::nextRetry(retryAfter);
    return status == CONTROL_READY;
  }

  // What the board reports: the forced value where one is forced, else its own output
  bool readBack(unsigned long now)
  {
    readAt = now;
    uint8_t got[OUTPUT_READ_BYTES];
    if (profile == nullptr || !command(OUTPUT_COMMAND_READ, nullptr, 0, got, sizeof(got)))
      return false;
    int v = got[profile->valveByte] ? VALVE_DHW : VALVE_SPACE;
    int p = got[profile->pumpByte] ? 1 : 0;
    if (v != reportedValve || p != reportedPump)
      changed = true;
    reportedValve = v;
    reportedPump = p;
    return true;
  }

  // Sends the outputs as wanted here: every output not forced from here goes back to the unit
  bool send(int valveWanted, int speedWanted)
  {
    uint8_t data[OUTPUT_FORCE_BYTES];
    memset(data, OUTPUT_NOT_FORCED, sizeof(data));
    if (valveWanted != VALVE_AUTO)
      data[profile->valveByte] = (uint8_t)valveWanted;
    if (speedWanted > 0)
    {
      uint16_t pwm = outputPumpPwm(speedWanted);
      data[profile->pumpByte] = 1;
      data[profile->pwmHighByte] = (uint8_t)(pwm >> 8);
      data[profile->pwmLowByte] = (uint8_t)pwm;
    }
    writes++;
    return command(OUTPUT_COMMAND_FORCE, data, sizeof(data), nullptr, 0);
  }

  bool apply(int valveWanted, int speedWanted, unsigned long now)
  {
    if (status != CONTROL_READY || !send(valveWanted, speedWanted))
      return false;
    valve = valveWanted;
    pumpSpeed = speedWanted;
    changed = true;
    readAt = now + 5000 - OUTPUT_CHECK_MS; // read back once the board has moved
    return true;
  }

  void requestValve(int v) { wantedValve = v; }
  void requestPump(int speed) { wantedPump = speed; }

  // The option was switched off. True while an output forced from here is still to be released: the caller keeps
  // ticking until it is.
  bool leaving()
  {
    if (!forcing() || (status != CONTROL_READY && status != CONTROL_NO_ANSWER))
      return false;
    if (status == CONTROL_READY)
    {
      wantedValve = VALVE_AUTO;
      wantedPump = 0;
    }
    return true;
  }

  // Runs the pending work: identification, the release at start, requests, the end of a hold, the read-back.
  // Owner task only.
  void tick(unsigned long now)
  {
    if (status != CONTROL_READY)
    {
      if (!identificationTried || now - checkedAt >= retryAfter)
        identify(now);
      return;
    }
    if (!cleared)
    {
      // An output forced before a restart of ESPAltherma would stay forced with nothing here to end it
      if (!send(VALVE_AUTO, 0))
      {
        identify(now);
        return;
      }
      cleared = true;
      readAt = now + 5000 - OUTPUT_CHECK_MS;
    }
    int wv = wantedValve, wp = wantedPump;
    if (wv != OUTPUT_NO_REQUEST || wp != OUTPUT_NO_REQUEST)
    {
      wantedValve = wantedPump = OUTPUT_NO_REQUEST;
      int v = wv != OUTPUT_NO_REQUEST ? wv : valve;
      int p = wp != OUTPUT_NO_REQUEST ? wp : pumpSpeed;
      if (apply(v, p, now))
      {
        forcedAt = now; // any request renews the hold
        return;
      }
      // Kept for when the unit answers again, unless something newer was asked meanwhile
      if (wantedValve == OUTPUT_NO_REQUEST)
        wantedValve = wv;
      if (wantedPump == OUTPUT_NO_REQUEST)
        wantedPump = wp;
      identify(now);
      return;
    }
    if (forcing() && now - forcedAt >= OUTPUT_HOLD_MS)
    {
      if (apply(VALVE_AUTO, 0, now))
        expired = true;
      else
        identify(now);
      return;
    }
    if (now - readAt >= OUTPUT_CHECK_MS)
    {
      if (!readBack(now))
      {
        identify(now);
        return;
      }
      // The unit restarted (or another tool talked to it): what is forced from here is forced again
      bool valveLost = valve != VALVE_AUTO && reportedValve != valve;
      bool pumpLost = pumpSpeed > 0 && reportedPump != 1;
      if (valveLost || pumpLost)
        send(valve, pumpSpeed);
    }
    else if (!forcing() && now - checkedAt >= CONTROL_CHECK_INTERVAL_MS)
      identify(now);
  }
};

#endif
