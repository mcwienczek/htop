/*
htop - darwin/PowerMeter.c
(C) 2026 htop dev team
Released under the GNU GPLv2+, see the COPYING file
in the source distribution for its full text.
*/

#include "config.h" // IWYU pragma: keep

#include "darwin/PowerMeter.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "CRT.h"
#include "Macros.h"
#include "Object.h"
#include "RichString.h"
#include "XUtils.h"

// Compatibility for older SDKs.
#if !defined(HAVE_DECL_KIOMAINPORTDEFAULT) || !HAVE_DECL_KIOMAINPORTDEFAULT
#define kIOMainPortDefault kIOMasterPortDefault
#endif

// Battery power (in W) below which the battery is considered idle,
// so that sensor noise on a held or full battery is not reported as draining.
#define POWER_IDLE_THRESHOLD 0.5

typedef struct PowerData_ {
   bool valid;
   bool onAC;
   bool fullyCharged;
   double percent;
   double adapterWatts;  // rated adapter power, NAN if unknown
   double inputWatts;    // power currently drawn from the adapter
   double systemWatts;   // power currently consumed by the system
   double batteryWatts;  // > 0 charging, < 0 discharging
} PowerData;

static PowerData power;

static const int PowerMeter_attributes[] = {
   METER_TEXT,
};

static bool getNumber(CFDictionaryRef dict, const char* key, double* out) {
   CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
   CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
   CFRelease(cfKey);

   if (!value || CFGetTypeID(value) != CFNumberGetTypeID())
      return false;

   // signed values (e.g. a negative Amperage) are stored as 64-bit two's complement
   int64_t tmp;
   if (!CFNumberGetValue(value, kCFNumberSInt64Type, &tmp))
      return false;

   *out = (double)tmp;
   return true;
}

static bool getBool(CFDictionaryRef dict, const char* key) {
   CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
   CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
   CFRelease(cfKey);

   return value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
}

static CFDictionaryRef getDict(CFDictionaryRef dict, const char* key) {
   CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingASCII);
   CFTypeRef value = CFDictionaryGetValue(dict, cfKey);
   CFRelease(cfKey);

   return (value && CFGetTypeID(value) == CFDictionaryGetTypeID()) ? value : NULL;
}

static void PowerMeter_readBattery(PowerData* data) {
   *data = (PowerData) {
      .valid = false,
      .percent = NAN,
      .adapterWatts = NAN,
      .inputWatts = NAN,
      .systemWatts = NAN,
      .batteryWatts = NAN,
   };

   io_service_t battery = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"));
   if (!battery)
      return;

   CFMutableDictionaryRef props = NULL;
   kern_return_t ret = IORegistryEntryCreateCFProperties(battery, &props, kCFAllocatorDefault, 0);
   IOObjectRelease(battery);
   if (ret != KERN_SUCCESS || !props)
      return;

   double current, max, voltage, amperage, tmp;

   if (getNumber(props, "CurrentCapacity", &current) && getNumber(props, "MaxCapacity", &max) && max > 0)
      data->percent = 100.0 * current / max;

   // mV * mA -> W
   if (getNumber(props, "Voltage", &voltage) && getNumber(props, "Amperage", &amperage))
      data->batteryWatts = voltage * amperage / 1e6;

   data->onAC = getBool(props, "ExternalConnected");
   data->fullyCharged = getBool(props, "FullyCharged");

   CFDictionaryRef adapter = getDict(props, "AdapterDetails");
   if (data->onAC && adapter && getNumber(adapter, "Watts", &tmp) && tmp > 0)
      data->adapterWatts = tmp;

   // Apple Silicon power telemetry, values in mW
   CFDictionaryRef telemetry = getDict(props, "PowerTelemetryData");
   if (telemetry) {
      if (getNumber(telemetry, "SystemPowerIn", &tmp))
         data->inputWatts = data->onAC ? tmp / 1000.0 : 0.0;
      if (getNumber(telemetry, "SystemLoad", &tmp))
         data->systemWatts = tmp / 1000.0;
   }

   data->valid = isNonnegative(data->percent) && !isnan(data->batteryWatts);

   CFRelease(props);
}

static void PowerMeter_updateValues(Meter* this) {
   PowerMeter_readBattery(&power);

   if (!power.valid) {
      xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "N/A");
      return;
   }

   xSnprintf(this->txtBuffer, sizeof(this->txtBuffer), "%.1f%% %+.1fW", power.percent, power.batteryWatts);
}

static void appendWatts(RichString* out, double watts, int precision) {
   char buffer[16];
   int len = isnan(watts) ? xSnprintf(buffer, sizeof(buffer), "?") : xSnprintf(buffer, sizeof(buffer), "%.*fW", precision, watts);
   RichString_appendnAscii(out, CRT_colors[METER_VALUE], buffer, len);
}

static void PowerMeter_display(ATTR_UNUSED const Object* cast, RichString* out) {
   char buffer[32];
   int len;

   if (!power.valid) {
      RichString_writeAscii(out, CRT_colors[METER_VALUE_ERROR], "N/A");
      return;
   }

   xSnprintf(buffer, sizeof(buffer), "%.1f%% ", power.percent);
   RichString_writeAscii(out, CRT_colors[METER_VALUE], buffer);

   const char* state;
   int color;
   if (power.batteryWatts >= POWER_IDLE_THRESHOLD) {
      state = "charging";
      color = METER_VALUE_OK;
   } else if (power.batteryWatts > -POWER_IDLE_THRESHOLD) {
      state = !power.onAC ? "idle" : power.fullyCharged ? "full" : "not charging";
      color = METER_VALUE;
   } else if (power.onAC) {
      state = "DRAINING ON AC";
      color = METER_VALUE_ERROR;
   } else {
      state = "discharging";
      color = METER_VALUE_WARN;
   }
   RichString_appendAscii(out, CRT_colors[color], state);

   if (fabs(power.batteryWatts) >= POWER_IDLE_THRESHOLD) {
      len = xSnprintf(buffer, sizeof(buffer), " %+.1fW", power.batteryWatts);
      RichString_appendnAscii(out, CRT_colors[color], buffer, len);
   }

   RichString_appendAscii(out, CRT_colors[METER_TEXT], "; AC: ");
   if (power.onAC) {
      appendWatts(out, power.inputWatts, 1);
      RichString_appendAscii(out, CRT_colors[METER_TEXT], " of ");
      appendWatts(out, power.adapterWatts, 0);
   } else {
      RichString_appendAscii(out, CRT_colors[METER_VALUE], "unplugged");
   }

   RichString_appendAscii(out, CRT_colors[METER_TEXT], "; system: ");
   appendWatts(out, power.systemWatts, 1);
}

const MeterClass PowerMeter_class = {
   .super = {
      .extends = Class(Meter),
      .delete = Meter_delete,
      .display = PowerMeter_display,
   },
   .updateValues = PowerMeter_updateValues,
   .defaultMode = TEXT_METERMODE,
   .supportedModes = (1 << TEXT_METERMODE),
   .maxItems = 0,
   .total = 0.0,
   .attributes = PowerMeter_attributes,
   .name = "Power",
   .uiName = "Power",
   .description = "Battery charge/discharge rate, AC adapter input and system power draw",
   .caption = "Power: "
};
