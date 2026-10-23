#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::buienradar {

// What a tile hands the card it opens: the card gets the tile's data, not its options or the frame it was showing.
struct Handoff {
  float latitude = 52.1f, longitude = 5.2f;
  int frame = 0;      // the map's frame, 0 for now
  int pace = 2;       // seconds per frame
  char problem[96] = {};   // what to type for the place, when Buienradar would not take it as typed
};

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tile and its card in setup().
class Buienradar : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  Handoff handoff;
};

}  // namespace esphome::buienradar
