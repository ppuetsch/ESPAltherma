#ifndef ESPALTHERMA_HPCONTROL_H
#define ESPALTHERMA_HPCONTROL_H
// Included after the controls/readers. Name each blocking step for the existing watchdog history.
void controlLoop()
{
  HP_STAGE("hot water target");
  controlTick(dhwControl, config.dhwTarget, "Hot water");
  HP_STAGE("leaving water target");
  controlTick(lwtControl, config.lwtTarget, "Leaving water");
  HP_STAGE("power limit");
  limitTick();
  HP_STAGE("unit counters");
  meterTick();
  HP_STAGE("memory read");
  memoryTick();
  HP_STAGE("memory dump");
  memoryDumpTick();
  HP_STAGE("save held targets");
  heldSaveTick();
  // Only on the HT outdoor unit: the hydrobox ports and other units are not read by this reader.
  bool diiiOn = config.protocol == 'I' && strstr(config.model, "ERRQ-ERSQ outdoor") != nullptr;
  DiiiStatus before = diii.status;
  HP_STAGE("DIII read");
  if (!diiiOn)
  {
    if (diii.status != DIII_OFF)
      diii.reset();
  }
  else if (!busy)
    diii.tick();
  if ((before == DIII_READY) != (diii.status == DIII_READY))
  {
    HP_STAGE("extra sensor list");
    haExtraSetup();
    discoveryDirty = true;
  }
}
#endif
