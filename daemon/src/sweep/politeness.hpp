#pragma once

namespace macassist {

// Reads system-pressure signals so background sweeps can yield to the user.
// A thin wrapper over NSProcessInfo (thermal) and IOKit (power source); the
// queries are cheap, so callers may poll them in a loop.
class Politeness {
 public:
  // `pretend_battery` (the hidden --pretend-battery dev flag) forces OnBattery
  // to report true, so the on-battery extraction backlog policy can be tested
  // on AC power. Set once at construction, before any thread reads it.
  explicit Politeness(bool pretend_battery = false)
      : pretend_battery_(pretend_battery) {}

  // True when thermal pressure is serious or critical -- the point at which a
  // background sweep should back off.
  bool UnderThermalPressure() const;

  // True when running on battery (no AC). Used to throttle heavy extraction;
  // the cheap stat-crawl ignores it.
  bool OnBattery() const;

  // Policy for the crawl / reconciliation: pause only under thermal pressure
  // (a stat-walk is cheap, so battery alone does not pause it).
  bool ShouldPauseSweep() const { return UnderThermalPressure(); }

 private:
  bool pretend_battery_ = false;
};

}  // namespace macassist
