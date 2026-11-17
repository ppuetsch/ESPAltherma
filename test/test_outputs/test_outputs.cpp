#include <string.h>
#include <unity.h>

#include "outputs.h"

// Fake hydro board 20002203: answers 'L' (its software), 'B' (force outputs) and 'S' (read them back). The board's
// own outputs stand for what its control does; a forced byte replaces them, 0xFF gives them back.
struct FakeBoard
{
    uint8_t softwareId[4];
    uint8_t forced[OUTPUT_FORCE_BYTES];
    uint8_t ownValve = 0, ownPump = 0;
    bool answer = true;
    int forceFrames = 0;
    uint8_t lastForce[OUTPUT_FORCE_BYTES];

    void reset()
    {
        const uint8_t id[4] = {0x03, 0x22, 0x00, 0x20}; // reads "20002203"
        memcpy(softwareId, id, 4);
        memset(forced, OUTPUT_NOT_FORCED, sizeof(forced));
        memset(lastForce, 0, sizeof(lastForce));
        ownValve = ownPump = 0;
        answer = true;
        forceFrames = 0;
    }

    void restart() { memset(forced, OUTPUT_NOT_FORCED, sizeof(forced)); }

    static uint8_t checksum(const uint8_t *b, size_t n)
    {
        uint8_t s = 0;
        for (size_t i = 0; i < n; i++)
            s += b[i];
        return (uint8_t)~s;
    }

    uint8_t reported(int index, uint8_t own) const { return forced[index] != OUTPUT_NOT_FORCED ? forced[index] : own; }

    size_t exchange(const uint8_t *frame, size_t len, uint8_t *reply, size_t maxLen)
    {
        if (!answer || len < 3 || frame[0] != len - 1 || checksum(frame, len - 1) != frame[len - 1])
            return 0;
        uint8_t cmd = frame[1];
        size_t n = len - 3, out = 0;
        reply[out++] = cmd;
        if (cmd == 0x4C && n == 0)
        {
            memcpy(reply + out, softwareId, 4);
            out += 4;
        }
        else if (cmd == OUTPUT_COMMAND_FORCE && n == OUTPUT_FORCE_BYTES)
        {
            memcpy(forced, frame + 2, OUTPUT_FORCE_BYTES);
            memcpy(lastForce, frame + 2, OUTPUT_FORCE_BYTES);
            forceFrames++;
        }
        else if (cmd == OUTPUT_COMMAND_READ && n == 0)
        {
            memset(reply + out, 0, OUTPUT_READ_BYTES);
            reply[out + 0] = reported(0, ownPump);
            reply[out + 5] = reported(5, ownValve);
            out += OUTPUT_READ_BYTES;
        }
        else
        {
            reply[0] = 0x15; // NAK
            return 1;
        }
        reply[out] = checksum(reply, out);
        out++;
        return out > maxLen ? maxLen : out;
    }
};

static FakeBoard board;

static size_t fakeExchange(const uint8_t *frame, size_t len, uint8_t *reply, size_t maxLen, unsigned long)
{
    return board.exchange(frame, len, reply, maxLen);
}

static OutputControl fresh()
{
    OutputControl c;
    c.exchange = fakeExchange;
    return c;
}

// Identified and released once, as after a start of ESPAltherma
static OutputControl ready(unsigned long now = 1000)
{
    OutputControl c = fresh();
    c.tick(now);
    c.tick(now + 10);
    return c;
}

static void assertNothingForced()
{
    for (int i = 0; i < OUTPUT_FORCE_BYTES; i++)
        TEST_ASSERT_EQUAL_HEX8(OUTPUT_NOT_FORCED, board.forced[i]);
}

void setUp() { board.reset(); }
void tearDown() {}

void test_pump_pwm_follows_the_speed()
{
    TEST_ASSERT_EQUAL_UINT16(29490, outputPumpPwm(55)); // the unit's own actuator test: 45 % signal
    TEST_ASSERT_EQUAL_UINT16(0, outputPumpPwm(100));
    TEST_ASSERT_EQUAL_UINT16(outputPumpPwm(OUTPUT_PUMP_SPEED_MIN), outputPumpPwm(1));
    TEST_ASSERT_EQUAL_UINT16(0, outputPumpPwm(150));
}

void test_valve_names_parse_and_print()
{
    TEST_ASSERT_EQUAL(VALVE_AUTO, valveForceParse("auto"));
    TEST_ASSERT_EQUAL(VALVE_SPACE, valveForceParse("space"));
    TEST_ASSERT_EQUAL(VALVE_DHW, valveForceParse("dhw"));
    TEST_ASSERT_EQUAL(OUTPUT_NO_REQUEST, valveForceParse("DHW "));
    TEST_ASSERT_EQUAL_STRING("dhw", valveForceName(VALVE_DHW));
    TEST_ASSERT_EQUAL_STRING("auto", valveForceName(VALVE_AUTO));
}

void test_another_software_gets_nothing()
{
    const uint8_t id[4] = {0x04, 0x29, 0x00, 0x18}; // 18002904
    memcpy(board.softwareId, id, 4);
    OutputControl c = fresh();
    c.requestValve(VALVE_DHW);
    c.tick(1000);
    c.tick(2000);
    TEST_ASSERT_EQUAL(CONTROL_UNSUPPORTED, c.status);
    TEST_ASSERT_EQUAL(0, board.forceFrames);
}

void test_outputs_are_released_once_at_start()
{
    board.forced[5] = VALVE_DHW; // left from before a restart of ESPAltherma
    OutputControl c = fresh();
    c.tick(1000);
    TEST_ASSERT_EQUAL(CONTROL_READY, c.status);
    TEST_ASSERT_EQUAL(VALVE_DHW, c.reportedValve);
    c.tick(1010);
    TEST_ASSERT_EQUAL(1, board.forceFrames);
    assertNothingForced();
    c.tick(1020);
    TEST_ASSERT_EQUAL(1, board.forceFrames);
}

void test_valve_is_forced_and_read_back()
{
    OutputControl c = ready();
    c.requestValve(VALVE_DHW);
    c.tick(2000);
    TEST_ASSERT_EQUAL(VALVE_DHW, c.valve);
    TEST_ASSERT_TRUE(c.forcing());
    TEST_ASSERT_EQUAL_HEX8(1, board.lastForce[5]);
    TEST_ASSERT_EQUAL_HEX8(OUTPUT_NOT_FORCED, board.lastForce[0]); // the pump stays with the unit
    TEST_ASSERT_EQUAL_HEX8(OUTPUT_NOT_FORCED, board.lastForce[7]);
    c.tick(2000 + 5000);
    TEST_ASSERT_EQUAL(VALVE_DHW, c.reportedValve);
}

void test_pump_needs_its_request_and_a_signal()
{
    OutputControl c = ready();
    c.requestValve(VALVE_DHW);
    c.tick(2000);
    c.requestPump(55);
    c.tick(3000);
    TEST_ASSERT_EQUAL(55, c.pumpSpeed);
    TEST_ASSERT_EQUAL_HEX8(1, board.lastForce[0]);
    TEST_ASSERT_EQUAL_HEX8(0x73, board.lastForce[7]); // 29490, high byte first
    TEST_ASSERT_EQUAL_HEX8(0x32, board.lastForce[8]);
    TEST_ASSERT_EQUAL_HEX8(1, board.lastForce[5]);    // the valve stays forced
    c.requestPump(0);
    c.tick(4000);
    TEST_ASSERT_EQUAL(0, c.pumpSpeed);
    TEST_ASSERT_EQUAL_HEX8(OUTPUT_NOT_FORCED, board.lastForce[0]);
    TEST_ASSERT_EQUAL_HEX8(OUTPUT_NOT_FORCED, board.lastForce[7]);
    TEST_ASSERT_EQUAL_HEX8(1, board.lastForce[5]);
}

void test_a_hold_ends_by_itself_and_a_request_renews_it()
{
    OutputControl c = ready();
    c.requestValve(VALVE_DHW);
    c.tick(2000);
    c.requestValve(VALVE_DHW); // the automation asks again half way
    c.tick(2000 + OUTPUT_HOLD_MS / 2);
    c.tick(2000 + OUTPUT_HOLD_MS + 10);
    TEST_ASSERT_TRUE(c.forcing());
    TEST_ASSERT_FALSE(c.expired);
    c.tick(2000 + OUTPUT_HOLD_MS / 2 + OUTPUT_HOLD_MS);
    TEST_ASSERT_FALSE(c.forcing());
    TEST_ASSERT_TRUE(c.expired);
    assertNothingForced();
}

void test_an_output_lost_in_a_restart_of_the_unit_is_forced_again()
{
    OutputControl c = ready();
    c.requestValve(VALVE_DHW);
    c.requestPump(60);
    c.tick(2000);
    int frames = board.forceFrames;
    board.restart();
    c.tick(2000 + 5000); // the read-back after the write
    TEST_ASSERT_EQUAL(frames + 1, board.forceFrames);
    TEST_ASSERT_EQUAL_HEX8(1, board.forced[5]);
    TEST_ASSERT_EQUAL_HEX8(1, board.forced[0]);
}

void test_leaving_releases_what_was_forced()
{
    OutputControl c = ready();
    c.requestValve(VALVE_SPACE);
    c.tick(2000);
    TEST_ASSERT_TRUE(c.leaving());
    c.tick(3000);
    TEST_ASSERT_FALSE(c.forcing());
    TEST_ASSERT_FALSE(c.leaving());
    assertNothingForced();
}

void test_no_answer_keeps_the_request_until_the_unit_is_back()
{
    OutputControl c = ready();
    board.answer = false;
    c.requestValve(VALVE_DHW);
    c.tick(2000);
    TEST_ASSERT_EQUAL(CONTROL_NO_ANSWER, c.status);
    TEST_ASSERT_EQUAL(VALVE_AUTO, c.valve);
    board.answer = true;
    c.tick(2000 + CONTROL_RETRY_FIRST_MS);
    c.tick(2000 + CONTROL_RETRY_FIRST_MS + 10);
    TEST_ASSERT_EQUAL(CONTROL_READY, c.status);
    TEST_ASSERT_EQUAL(VALVE_DHW, c.valve);
    TEST_ASSERT_EQUAL_HEX8(1, board.forced[5]);
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_pump_pwm_follows_the_speed);
    RUN_TEST(test_valve_names_parse_and_print);
    RUN_TEST(test_another_software_gets_nothing);
    RUN_TEST(test_outputs_are_released_once_at_start);
    RUN_TEST(test_valve_is_forced_and_read_back);
    RUN_TEST(test_pump_needs_its_request_and_a_signal);
    RUN_TEST(test_a_hold_ends_by_itself_and_a_request_renews_it);
    RUN_TEST(test_an_output_lost_in_a_restart_of_the_unit_is_forced_again);
    RUN_TEST(test_leaving_releases_what_was_forced);
    RUN_TEST(test_no_answer_keeps_the_request_until_the_unit_is_back);
    return UNITY_END();
}
