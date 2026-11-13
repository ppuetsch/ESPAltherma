#include <unity.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <string.h>
#include "stage.h"
static std::vector<std::string> stages;
static bool stallMeter=false,busy=false,discoveryDirty=false;
static struct {bool dhwTarget=true,lwtTarget=true;char protocol='I';char model[80]="ERRQ-ERSQ outdoor";} config;
static struct Control{} dhwControl,lwtControl;
enum DiiiStatus {DIII_OFF,DIII_READY};
static void record(){stages.push_back((const char*)hpStage);}
void controlTick(Control&,bool,const char*){record();}
void limitTick(){record();}void memoryTick(){record();}void memoryDumpTick(){record();}void heldSaveTick(){record();}
void meterTick(){record();if(stallMeter)throw std::runtime_error("simulated blocked meter call");}
void haExtraSetup(){record();}
static struct {DiiiStatus status=DIII_READY;void reset(){status=DIII_OFF;}void tick(){record();status=DIII_READY;}} diii;
#include "hpcontrol.h"
void setUp(){stages.clear();stallMeter=false;diii.status=DIII_READY;discoveryDirty=false;}
void tearDown(){}
void test_each_blocking_step_publishes_its_own_watchdog_stage()
{
    controlLoop();const char *expected[]={"hot water target","leaving water target","power limit","unit counters","memory read","memory dump","save held targets","DIII read"};
    TEST_ASSERT_EQUAL(8,stages.size());for(size_t i=0;i<stages.size();i++) TEST_ASSERT_EQUAL_STRING(expected[i],stages[i].c_str());
    stages.clear();diii.status=DIII_OFF;controlLoop();TEST_ASSERT_TRUE(discoveryDirty);TEST_ASSERT_EQUAL_STRING("extra sensor list",stages.back().c_str());
}
void test_interrupted_counter_read_keeps_correct_watchdog_cause()
{
    stallMeter=true;bool interrupted=false;try{controlLoop();}catch(const std::runtime_error&){interrupted=true;}
    TEST_ASSERT_TRUE(interrupted);TEST_ASSERT_EQUAL_STRING("unit counters",(const char*)hpStage);TEST_ASSERT_EQUAL(4,stages.size());
}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_each_blocking_step_publishes_its_own_watchdog_stage);RUN_TEST(test_interrupted_counter_read_keeps_correct_watchdog_cause);return UNITY_END();}
