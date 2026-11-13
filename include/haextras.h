#ifndef ESPALTHERMA_HAEXTRAS_H
#define ESPALTHERMA_HAEXTRAS_H
// Included after the feature labels/readers. The same lock protects construction and discovery capture.
void haExtraSetup()
{
  valuesLock();
  static LabelDef extra[HA_EXTRA_LABELS_MAX];
  size_t n = 0;
  if (config.statsEnabled)
    for (const LabelDef &l : STATS_LABELS)
      extra[n++] = l;
  if (diii.status == DIII_READY)
    for (const LabelDef &l : DIII_LABELS)
      extra[n++] = l;
  if (unitMeter.status == CONTROL_READY)
    extra[n++] = METER_LABEL;
  if (unitMeter.model[0])
    extra[n++] = UNIT_MODEL_LABEL;
  static_assert(sizeof(STATS_LABELS) / sizeof(STATS_LABELS[0]) + sizeof(DIII_LABELS) / sizeof(DIII_LABELS[0]) + 2 + UNIT_COUNTER_COUNT <= HA_EXTRA_LABELS_MAX, "room for the unit's counters");
  for (size_t i = 0; unitMeter.status == CONTROL_READY && i < UNIT_COUNTER_COUNT; i++)
    if (unitMeter.counterMapped(i))
      extra[n++] = LabelDef(-1, -1, -1, -1, unitCounterSensorType(UNIT_COUNTER_DEFS[i]), UNIT_COUNTER_DEFS[i].label);
  haExtraLabels = n ? extra : nullptr;
  haExtraCount = n;
  valuesUnlock();
}
#endif
