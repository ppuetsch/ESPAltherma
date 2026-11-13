#include <unity.h>
#include <mutex>
#include <future>
#include <chrono>
#include "labeldef.h"
#include "control.h"
static std::mutex valuesMutex;
void valuesLock(){valuesMutex.lock();}void valuesUnlock(){valuesMutex.unlock();}
static struct {bool statsEnabled=false;} config;
static const int DIII_READY=2;
static struct {int status=0;} diii;
static MeterReader unitMeter;
static const LabelDef STATS_LABELS[]={{-1,-1,-1,-1,989,"Stats heat"}};
static const LabelDef DIII_LABELS[]={{-1,-1,-1,-1,1,"Outdoor"}};
static const LabelDef METER_LABEL={-1,-1,-1,-1,990,"Meter"},UNIT_MODEL_LABEL={-1,-1,-1,-1,986,"Model"};
static const LabelDef *haExtraLabels=nullptr;
static size_t haExtraCount=0;
static const int HA_EXTRA_LABELS_MAX=96;
static int unitCounterSensorType(const UnitCounterDef&){return 990;}
#include "haextras.h" // the production writer, with small feature-label fixtures
void setUp(){config.statsEnabled=false;diii.status=0;unitMeter=MeterReader();}
void tearDown(){}
void test_extra_writer_waits_for_discovery_reader_then_publishes_complete_list()
{
    config.statsEnabled=true;haExtraSetup();
    valuesLock();
    std::promise<void> started;
    auto writer=std::async(std::launch::async,[&](){started.set_value();diii.status=DIII_READY;haExtraSetup();});
    started.get_future().wait();
    bool blocked=writer.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout;
    size_t before=haExtraCount;
    valuesUnlock();writer.get(); // clean up even if the assertion will fail against the original writer
    TEST_ASSERT_TRUE(blocked);TEST_ASSERT_EQUAL(1,before);TEST_ASSERT_EQUAL(2,haExtraCount);
    TEST_ASSERT_EQUAL_STRING("Stats heat",haExtraLabels[0].label);TEST_ASSERT_EQUAL_STRING("Outdoor",haExtraLabels[1].label);
    config.statsEnabled=false;diii.status=0;haExtraSetup();
    TEST_ASSERT_EQUAL(0,haExtraCount);TEST_ASSERT_NULL(haExtraLabels);
}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_extra_writer_waits_for_discovery_reader_then_publishes_complete_list);return UNITY_END();}
