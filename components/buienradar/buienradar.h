#pragma once
#include "esphome/core/component.h"
#include "esphome/components/select/select.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::buienradar {

// What a tile hands the card it opens: the card gets the tile's data, not its options or the frame it was showing.
struct Handoff {
  float latitude = 52.1f, longitude = 5.2f;
  int frame = 0;      // the map's frame, 0 for now
  int pace = 2;       // seconds per frame
  char problem[96] = {};   // what to type for the place, when Buienradar would not take it as typed
};

// The forecast a tile got last, for the top bar's item: a bar item has no data of its own, so it says what the
// screen's Buienradar tile knows (the newest of them, when there are several).
struct Latest {
  static constexpr int POINTS = 40;
  uint32_t at[POINTS] = {};
  float mm[POINTS] = {};
  int n = 0;
  uint32_t received = 0;   // the screen's clock when it came
};

// The plugin: an ESPHome component and a tessera::Plugin. It registers its tile, its card and its top bar item in
// setup(), and the item's two settings on the screen's settings page.
class Buienradar : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  bool settings(tessera::SettingsPage &page) override;

  void set_bar_when(select::Select *s) { bar_when_ = s; }
  void set_bar_words(select::Select *s) { bar_words_ = s; }

  Handoff handoff;
  Latest latest;

 protected:
  tessera::BarItem bar() const;
  select::Select *bar_when_{}, *bar_words_{};
};

}  // namespace esphome::buienradar
