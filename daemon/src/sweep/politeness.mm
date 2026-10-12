#include "sweep/politeness.hpp"

#import <Foundation/Foundation.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

namespace macassist {

bool Politeness::UnderThermalPressure() const {
  @autoreleasepool {
    const NSProcessInfoThermalState s =
        [[NSProcessInfo processInfo] thermalState];
    return s == NSProcessInfoThermalStateSerious ||
           s == NSProcessInfoThermalStateCritical;
  }
}

bool Politeness::OnBattery() const {
  CFTypeRef info = IOPSCopyPowerSourcesInfo();
  if (info == nullptr) return false;
  // The "providing" source is the one currently powering the machine; owned by
  // `info`, so it is released with it (no separate CFRelease).
  CFStringRef type = IOPSGetProvidingPowerSourceType(info);
  const bool on_battery =
      type != nullptr && CFStringCompare(type, CFSTR(kIOPSBatteryPowerValue),
                                         0) == kCFCompareEqualTo;
  CFRelease(info);
  return on_battery;
}

}  // namespace macassist
