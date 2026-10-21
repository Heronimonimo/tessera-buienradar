#include "buienradar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "esphome/core/hal.h"

namespace esphome::buienradar {

using tessera::Fit;
using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// ---- Buienradar's data ----

// The rain at the place, every five minutes: the fetch's answer has about 35 of them.
static constexpr int MAX_POINTS = 40;
static constexpr uint32_t STEP = 300;
// Below this many millimetres an hour it counts as dry, as Buienradar's own words do.
static constexpr float RAIN = 0.1f;

// The radar frames: from ten minutes before the newest to two hours after it, every five minutes. The newest frame is
// about ten minutes behind the clock, so the frame of a moment is asked for only once it exists, and the first frame
// lies about where the graph starts.
static constexpr int FIRST_FRAME = -2, LAST_FRAME = 24;
static constexpr uint32_t LAG = 600;

// The map: Buienradar's own background for its radar, the very area of the radar frames (longitude 0 to 10, latitude
// 49.5 to 54.8, in Web Mercator), 1058 by 916 pixels as the frames are 1058 by 915. Its radar page names it
// "MapWebmercatorNL?size=Full"; the image API answers that with a redirect to this file, which the app does not follow.
static const char *const BACKGROUND = "https://cdn.buienradar.nl/resources/image-api-backgrounds/map-nl-1058x916-webmercator_full.jpg";
static constexpr int MAP_W = 1058, MAP_H = 916;
// A tile or card at least this many percent as wide as high has the map beside the graph, else above it.
static constexpr int WIDE = 125;
// One at least this many percent as wide as high, and not wide, is square: the map large with the words or keys beside
// it and a lower graph under both. Narrower is tall: the map on top, the rest under it.
static constexpr int SQUARE = 80;
static constexpr float WEST = 0.0f, EAST = 10.0f, NORTH = 54.8f, SOUTH = 49.5f;

// Icons of Tessera's set.
static constexpr uint32_t ICON_PLAY = 0xF040A, ICON_PAUSE = 0xF03E4, ICON_PREVIOUS = 0xF0141, ICON_NEXT = 0xF0142;

static void shown(lv_obj_t *obj, bool on) {
  if (!obj) return;
  if (on) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// A day since 1970 as year, month and day (Howard Hinnant's civil_from_days).
static void civil(long days, int &y, unsigned &m, unsigned &d) {
  days += 719468;
  const long era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (m <= 2 ? 1 : 0);
}

// A moment as Buienradar names its frames: YYYYMMDDHHMM in UTC.
static std::string stamp(uint32_t t) {
  int y = 0;
  unsigned m = 0, d = 0;
  civil(static_cast<long>(t / 86400), y, m, d);
  const unsigned s = t % 86400;
  char out[16];
  snprintf(out, sizeof(out), "%04d%02u%02u%02u%02u", y, m, d, s / 3600, (s / 60) % 60);
  return out;
}

// The newest radar frame that exists at this moment; 0 while the clock is not set.
static uint32_t newest_frame(uint32_t epoch) { return epoch > LAG ? (epoch - LAG) / STEP * STEP : 0; }

static uint32_t frame_time(uint32_t newest, int frame) {
  return static_cast<uint32_t>(static_cast<int64_t>(newest) + static_cast<int64_t>(frame) * STEP);
}

// The radar frame `frame` steps from the newest: a measured one up to the newest, the forecast made then after it.
static std::string frame_url(uint32_t newest, int frame) {
  const uint32_t t = frame_time(newest, frame);
  if (frame <= 0) return "https://processing-cdn.buienradar.nl/processing/nl/rain/current/webm/" + stamp(t) + ".png";
  return "https://processing-cdn.buienradar.nl/processing/nl/rain/forecast/runs/webm/" + stamp(newest) + "/" +
         stamp(t) + ".png";
}

static std::string millimetres(float mm) {
  char out[12];
  snprintf(out, sizeof(out), "%.1f", mm);
  return out;
}

// The forecast at the place, as the fetch mapped it: {"items": [{"at": <UTC seconds>, "mm": 0.4}, ...]}.
struct Forecast {
  uint32_t at[MAX_POINTS] = {};
  float mm[MAX_POINTS] = {};
  int n = 0;
  bool stale = false;
  char wait[16] = {};

  void read(JsonObjectConst data) {
    snprintf(wait, sizeof(wait), "%s", data["wait"] | "");
    stale = data["stale"] | false;
    n = 0;
    for (JsonObjectConst item : data["items"].as<JsonArrayConst>()) {
      const uint32_t t = item["at"] | 0u;
      if (!t || n >= MAX_POINTS) continue;
      at[n] = t;
      mm[n] = std::max(0.0f, item["mm"] | 0.0f);
      ++n;
    }
  }
  // The point whose five minutes hold `t`: the last that starts at or before it.
  int index(uint32_t t) const {
    int found = 0;
    for (int i = 0; i < n; ++i)
      if (at[i] <= t) found = i;
    return found;
  }
  bool covers(uint32_t t) const { return n > 0 && t + STEP / 2 >= at[0] && t < at[n - 1] + STEP; }
};

// What the tile and the card say in words: whether it rains now and until when, or when it starts, and how hard.
struct Summary {
  std::string head, sub;
  bool rain_later = false;
  int minutes = 0;   // until the rain starts, when it is dry now
};

static Summary summarise(const tessera::Plugin *p, const Forecast &f, uint32_t now) {
  Summary s;
  if (!f.n) {
    const std::string why = f.wait;
    s.head = p->text(why == "not_filled" ? "not_filled" : (why == "failed" || why == "refused" || why == "too_large")
                                                              ? "failed"
                                                              : "asking");
    return s;
  }
  const int i = now ? f.index(now) : 0;
  float most = 0;
  for (int j = i; j < f.n; ++j) most = std::max(most, f.mm[j]);
  if (f.mm[i] >= RAIN) {
    int end = -1;
    for (int j = i + 1; j < f.n && end < 0; ++j)
      if (f.mm[j] < RAIN) end = j;
    s.head = end < 0 ? p->text("rain_long") : tessera::fill(p->text("rain_until"), "time", tessera::clock_text(f.at[end]));
  } else {
    int start = -1;
    for (int j = i + 1; j < f.n && start < 0; ++j)
      if (f.mm[j] >= RAIN) start = j;
    if (start < 0) {
      s.head = p->text("dry_long");
    } else {
      s.head = tessera::fill(p->text("dry_until"), "time", tessera::clock_text(f.at[start]));
      s.rain_later = true;
      s.minutes = now && f.at[start] > now ? static_cast<int>((f.at[start] - now + 59) / 60) : 0;
    }
  }
  if (most >= RAIN) s.sub = tessera::fill(p->text("most"), "mm", millimetres(most));
  if (f.stale) s.sub = p->text("stale");
  return s;
}

// "now", "+15 min", "-10 min": a frame's moment against the clock, in steps of five minutes.
static std::string relative(const tessera::Plugin *p, uint32_t t, uint32_t now) {
  const long minutes = (static_cast<long>(t) - static_cast<long>(now)) / 60;
  const long rounded = (minutes >= 0 ? minutes + 2 : minutes - 2) / 5 * 5;
  if (rounded == 0) return p->text("now");
  return rounded > 0 ? tessera::format(p->text("later"), rounded) : tessera::format(p->text("earlier"), -rounded);
}

// ---- The graph: a bar per five minutes, a line at now, a cursor at the map's moment, the hours under it ----

class Graph {
 public:
  void make(lv_obj_t *parent, int x, int y, int w, int h, bool axis) {
    x_ = x;
    y_ = y;
    w_ = std::max(1, w);
    axis_ = axis && h > 3 * ui::line_height(Font::BODY);
    gh_ = std::max(1, h - (axis_ ? ui::line_height(Font::BODY) : 0));
    base_ = ui::block(parent, theme::LINE);
    lv_obj_set_style_radius(base_, 0, 0);
    lv_obj_set_size(base_, w_, std::max(1, ui::px(1)));
    lv_obj_set_pos(base_, x_, y_ + gh_ - std::max(1, ui::px(1)));
    for (auto &bar : bars_) {
      bar = ui::block(parent, theme::MARK_BLUE);
      lv_obj_set_style_radius(bar, std::max(1, ui::px(1)), 0);
      shown(bar, false);
    }
    now_ = ui::block(parent, theme::MUTED);
    lv_obj_set_style_radius(now_, 0, 0);
    lv_obj_set_size(now_, std::max(1, ui::px(1)), gh_);
    cursor_ = ui::block(parent, theme::INK);
    lv_obj_set_style_radius(cursor_, 0, 0);
    lv_obj_set_size(cursor_, std::max(2, ui::px(2)), gh_);
    shown(now_, false);
    shown(cursor_, false);
    if (axis_)
      for (auto &l : labels_) {
        l = ui::label(parent, Font::BODY, theme::MUTED);
        lv_obj_set_width(l, 1);
      }
  }

  // The forecast against the clock; `cursor` the map's moment, 0 for none.
  void paint(const tessera::Plugin *p, const Forecast &f, uint32_t now, uint32_t cursor) {
    if (!f.n) {
      for (auto *bar : bars_) shown(bar, false);
      shown(now_, false);
      shown(cursor_, false);
      for (auto *l : labels_) if (l) ui::set_text(l, "");
      t0_ = t1_ = 0;
      return;
    }
    t0_ = f.at[0];
    t1_ = f.at[f.n - 1] + STEP;
    const int slot = std::max(1, static_cast<int>(static_cast<int64_t>(w_) * STEP / (t1_ - t0_)));
    const int bar_w = std::max(1, slot > 3 ? slot * 3 / 4 : slot);
    for (int i = 0; i < MAX_POINTS; ++i) {
      lv_obj_t *bar = bars_[i];
      const float mm = i < f.n ? f.mm[i] : 0.0f;
      if (i >= f.n || mm < 0.01f) {
        shown(bar, false);
        continue;
      }
      // A log scale, as Buienradar's graph: a drizzle shows, a downpour does not flatten the rest.
      const float share = std::min(1.0f, std::max(0.06f, (std::log10(mm) + 1.3f) / 2.3f));
      const int h = std::max(1, static_cast<int>(share * gh_));
      lv_obj_set_size(bar, bar_w, h);
      lv_obj_set_pos(bar, x_of(f.at[i]) + (slot - bar_w) / 2, y_ + gh_ - h);
      ui::set_fill(bar, Tone::NORMAL, now && f.at[i] + STEP <= now ? theme::ACCENT_TINT : theme::MARK_BLUE);
      shown(bar, true);
    }
    const bool now_in = now && now >= t0_ && now < t1_;
    shown(now_, now_in);
    if (now_in) lv_obj_set_pos(now_, x_of(now), y_);
    // A frame before the graph's first bar stands at its start.
    const bool cursor_in = cursor && cursor < t1_;
    shown(cursor_, cursor_in);
    if (cursor_in) lv_obj_set_pos(cursor_, std::min(x_ + w_ - 2, std::max(x_, cursor < t0_ ? x_ : x_of(cursor))), y_);
    if (!axis_) return;
    for (int k = 0; k < 3; ++k) {
      lv_obj_t *l = labels_[k];
      const uint32_t t = now + static_cast<uint32_t>(k) * 3600;
      if (!now || t >= t1_) {
        ui::set_text(l, "");
        continue;
      }
      // As wide as its words, under its moment, kept inside the graph.
      const std::string words = k == 0 ? std::string(p->text("now")) : tessera::clock_text(t);
      const int lw = std::min(w_, ui::text_width(words, Font::BODY) + 2);
      lv_obj_set_width(l, lw);
      ui::set_text(l, words);
      lv_obj_set_pos(l, std::min(x_ + w_ - lw, std::max(x_, x_of(t) - lw / 2)), y_ + gh_);
    }
  }

  void colors() {
    ui::set_fill(base_, Tone::NORMAL, theme::LINE);
    ui::set_fill(now_, Tone::NORMAL, theme::MUTED);
    ui::set_fill(cursor_, Tone::NORMAL, theme::INK);
    for (auto *l : labels_) if (l) ui::set_color(l, theme::MUTED);
  }

  bool hit(const tessera::Point &p) const {
    return t1_ > t0_ && p.x >= x_ && p.x < x_ + w_ && p.y >= y_ && p.y < y_ + gh_ + (axis_ ? ui::line_height(Font::BODY) : 0);
  }
  uint32_t time_at(int px) const {
    return t0_ + static_cast<uint32_t>(static_cast<int64_t>(std::max(0, px - x_)) * (t1_ - t0_) / w_);
  }

 private:
  int x_of(uint32_t t) const {
    const int64_t d = static_cast<int64_t>(t) - static_cast<int64_t>(t0_);
    return x_ + static_cast<int>(d * w_ / static_cast<int64_t>(t1_ - t0_));
  }
  int x_ = 0, y_ = 0, w_ = 1, gh_ = 1;
  bool axis_ = false;
  uint32_t t0_ = 0, t1_ = 0;
  lv_obj_t *bars_[MAX_POINTS] = {};
  lv_obj_t *base_ = nullptr, *now_ = nullptr, *cursor_ = nullptr;
  lv_obj_t *labels_[3] = {};
};

// ---- The map: Buienradar's background, the radar frame over it, a dot at the place, the frame's time in a corner ----

class Map {
 public:
  // The largest map of the background's proportions within `room_w` by `room_h`, at x, y.
  static int fit_width(int room_w, int room_h) { return std::max(2, std::min(room_w, room_h * MAP_W / MAP_H)); }
  static int height_of(int width) { return std::max(2, width * MAP_H / MAP_W); }

  void make(lv_obj_t *parent, int x, int y, int width, float latitude, float longitude) {
    x_ = x;
    y_ = y;
    w_ = width;
    h_ = height_of(width);
    background_ = ui::picture(parent);
    lv_obj_set_style_radius(background_, 0, 0);
    lv_obj_set_pos(background_, x_, y_);
    lv_obj_set_size(background_, w_, h_);
    // The radar lies over the background, in the same place: a place made later is painted over the ones before it.
    radar_ = ui::picture(parent);
    lv_obj_set_style_radius(radar_, 0, 0);
    lv_obj_set_pos(radar_, x_, y_);
    lv_obj_set_size(radar_, w_, h_);
    // The place: a dot with a ring, where the latitude and longitude lie on the map.
    const int ring = std::max(6, ui::mm(2)), dot = std::max(4, ring * 6 / 10);
    ring_ = ui::block(parent, theme::CAMERA_INK);
    dot_ = ui::block(parent, theme::ACCENT);
    lv_obj_set_size(ring_, ring, ring);
    lv_obj_set_size(dot_, dot, dot);
    lv_obj_set_style_radius(ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    const float fx = (longitude - WEST) / (EAST - WEST);
    const float fy = (mercator(NORTH) - mercator(latitude)) / (mercator(NORTH) - mercator(SOUTH));
    const bool inside = fx >= 0 && fx < 1 && fy >= 0 && fy < 1;
    shown(ring_, inside);
    shown(dot_, inside);
    if (inside) {
      const int cx = x_ + static_cast<int>(fx * w_), cy = y_ + static_cast<int>(fy * h_);
      lv_obj_set_pos(ring_, cx - ring / 2, cy - ring / 2);
      lv_obj_set_pos(dot_, cx - dot / 2, cy - dot / 2);
    }
    // The frame's time, in the map's bottom left corner.
    badge_ = ui::block(parent, theme::CAMERA_PAGE);
    badge_text_ = ui::label(badge_, Font::BODY, theme::CAMERA_INK);
    lv_obj_set_pos(badge_text_, ui::px(6), 0);
    ui::set_picture(background_, BACKGROUND, Fit::COVER);
    colors();
  }

  void show(const tessera::Plugin *p, uint32_t newest, int frame, uint32_t now) {
    if (!newest) {
      shown(badge_, false);
      return;
    }
    ui::set_picture(radar_, frame_url(newest, frame), Fit::COVER);
    const uint32_t t = frame_time(newest, frame);
    const std::string words = tessera::clock_text(t) + "  " + relative(p, t, now);
    ui::set_text(badge_text_, words);
    const int lh = ui::line_height(Font::BODY), w = std::min(w_ - ui::px(8), ui::text_width(words, Font::BODY) + ui::px(12));
    lv_obj_set_size(badge_, w, lh + ui::px(2));
    lv_obj_set_width(badge_text_, w - ui::px(12));
    lv_obj_set_pos(badge_, x_ + ui::px(4), y_ + h_ - lh - ui::px(6));
    shown(badge_, true);
  }

  void colors() {
    ui::set_fill(ring_, Tone::NORMAL, theme::CAMERA_INK);
    ui::set_fill(dot_, Tone::ALERT, theme::ACCENT);
    ui::set_fill(badge_, Tone::NORMAL, theme::CAMERA_PAGE);
    ui::set_color(badge_text_, theme::CAMERA_INK);
  }

  bool hit(const tessera::Point &p) const { return p.x >= x_ && p.x < x_ + w_ && p.y >= y_ && p.y < y_ + h_; }
  int width() const { return w_; }
  int height() const { return h_; }

 private:
  static float mercator(float latitude) {
    return std::log(std::tan(3.14159265f / 4.0f + latitude * 3.14159265f / 360.0f));
  }
  int x_ = 0, y_ = 0, w_ = 2, h_ = 2;
  lv_obj_t *background_ = nullptr, *radar_ = nullptr, *ring_ = nullptr, *dot_ = nullptr, *badge_ = nullptr,
           *badge_text_ = nullptr;
};

// ---- The tile ----

// The graph on every size; on a tile of 3x2, 2x3 or larger (and a board with pictures) the map too, playing by itself.
// One cell says it in words over a small graph. A tap opens the card at the frame the map shows.
class RainTile : public tessera::Tile {
 public:
  explicit RainTile(Buienradar *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    tile_ = c.tile;
    snprintf(name_, sizeof(name_), "%s", c.name ? c.name : "");
    latitude_ = c.options["latitude"] | 52.1f;
    longitude_ = c.options["longitude"] | 5.18f;
    pace_ = std::max(1, std::min(10, c.options["pace"] | 2));
    const std::string play = c.options["play"] | "all";
    play_ = play == "still" ? Play::STILL : play == "forecast" ? Play::FORECAST : Play::ALL;
    map_ = (c.options["map"] | true) && ui::pictures() &&
           ((c.columns >= 3 && c.rows >= 2) || (c.columns >= 2 && c.rows >= 3) ||
            (c.columns >= 2 && c.rows >= 2 && std::min(c.width, c.height) >= ui::mm(35)));
    frame_ = first();

    const int w = c.width, h = c.height, gap = ui::px(6);
    const int title = ui::line_height(Font::TITLE), body = ui::line_height(Font::BODY);
    if (map_ && w * 100 >= h * WIDE) {
      // Wide: the map at the left over the whole height, the words and the graph beside it.
      const int mw = Map::fit_width(w * 60 / 100, h);
      map_view_.make(c.parent, 0, (h - Map::height_of(mw)) / 2, mw, latitude_, longitude_);
      const int x = mw + gap * 2, top = title + body + gap;
      words(c.parent, x, 0, w - x, title, body);
      graph_.make(c.parent, x, top, w - x, h - top, true);
    } else if (map_ && w * 100 >= h * SQUARE) {
      // Square: the map large at the top left, the words beside it, a lower graph across the bottom.
      const int graph = std::max(h * 24 / 100, 3 * body + ui::px(4));
      const int mw = Map::fit_width(w * 70 / 100, h - graph - gap), mh = Map::height_of(mw);
      map_view_.make(c.parent, 0, 0, mw, latitude_, longitude_);
      column(c.parent, mw + gap * 2, 0, w - mw - gap * 2, mh);
      graph_.make(c.parent, 0, mh + gap, w, h - mh - gap, true);
    } else if (map_) {
      // Tall: the map on top over the whole width, the words and the graph under it.
      const int mw = Map::fit_width(w, h * 55 / 100), mh = Map::height_of(mw);
      map_view_.make(c.parent, (w - mw) / 2, 0, mw, latitude_, longitude_);
      words(c.parent, 0, mh + gap, w, title, body);
      const int top = mh + gap + title + body + gap;
      graph_.make(c.parent, 0, top, w, h - top, true);
    } else if (c.columns == 1 && c.rows == 1) {
      // One cell: the words over two lines, a small graph under them.
      head_ = ui::text(c.parent, Font::BODY_LARGE, theme::INK);
      lv_obj_set_size(head_, w, 2 * ui::line_height(Font::BODY_LARGE));
      lv_obj_set_pos(head_, 0, 0);
      sub_ = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_width(sub_, w);
      lv_obj_set_pos(sub_, 0, 2 * ui::line_height(Font::BODY_LARGE));
      const int spark = std::max(ui::px(10), h / 4);
      graph_.make(c.parent, 0, h - spark, w, spark, false);
      one_cell_ = true;
    } else {
      // The graph alone: the words on top, the graph under them with the hours.
      const bool room = h - title - body - gap >= 3 * body;
      words(c.parent, 0, 0, w, title, room ? body : 0);
      const int top = title + (room ? body : 0) + gap;
      graph_.make(c.parent, 0, top, w, h - top, true);
    }
  }

  void on_state(JsonObjectConst data) override {
    forecast_.read(data);
    paint();
  }

  void on_tick(uint32_t epoch) override {
    now_ = epoch;
    bool changed = false;
    const uint32_t newest = newest_frame(epoch);
    if (newest != newest_) {
      newest_ = newest;
      changed = true;
    }
    // The map plays by itself: a frame every `pace` seconds, twice as long on now and on the last frame.
    if (map_ && play_ != Play::STILL && newest_) {
      const uint32_t ms = esphome::millis();
      if (!moved_) moved_ = ms;
      const uint32_t hold = static_cast<uint32_t>(pace_) * 1000u * ((frame_ == 0 || frame_ == LAST_FRAME) ? 2u : 1u);
      if (ms - moved_ >= hold) {
        frame_ = frame_ >= LAST_FRAME ? first() : frame_ + 1;
        moved_ = ms;
        changed = true;
      }
    }
    if (epoch / 60 != minute_) {
      minute_ = epoch / 60;
      changed = true;
    }
    if (changed) paint();
  }

  void on_theme() override {
    if (head_) ui::set_color(head_, theme::INK);
    if (sub_) ui::set_color(sub_, theme::MUTED);
    graph_.colors();
    if (map_) map_view_.colors();
  }

  void on_tap() override {
    plugin_->handoff.latitude = latitude_;
    plugin_->handoff.longitude = longitude_;
    plugin_->handoff.frame = map_ ? frame_ : 0;
    plugin_->handoff.pace = pace_;
    tessera::open_card(plugin_->plugin_id(), "radar", "", tile_, name_[0] ? std::string(name_) : plugin_->text("title"));
  }

 private:
  enum class Play : uint8_t { ALL, FORECAST, STILL };

  int first() const { return play_ == Play::ALL ? FIRST_FRAME : 0; }

  // The words in a narrow column beside the map: each over as many lines as it needs.
  void column(lv_obj_t *parent, int x, int y, int w, int h) {
    const int title = ui::line_height(Font::TITLE), body = ui::line_height(Font::BODY);
    head_ = ui::text(parent, Font::TITLE, theme::INK);
    lv_obj_set_size(head_, std::max(1, w), std::min(h, 2 * title));
    lv_obj_set_pos(head_, x, y);
    sub_ = ui::text(parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(sub_, std::max(1, w), std::max(body, std::min(h - 2 * title - ui::px(4), 4 * body)));
    lv_obj_set_pos(sub_, x, y + 2 * title + ui::px(4));
  }

  void words(lv_obj_t *parent, int x, int y, int w, int title, int body) {
    head_ = ui::label(parent, Font::TITLE, theme::INK);
    lv_obj_set_width(head_, std::max(1, w));
    lv_obj_set_pos(head_, x, y);
    if (!body) return;
    sub_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_width(sub_, std::max(1, w));
    lv_obj_set_pos(sub_, x, y + title);
  }

  void paint() {
    const Summary s = summarise(plugin_, forecast_, now_);
    if (head_) ui::set_text(head_, s.head);
    if (sub_) {
      // Buienradar's data is free with its source named: the second line says it, beside how hard it rains.
      const std::string source = plugin_->text("source");
      std::string sub = s.sub.empty() ? source : s.sub + " · " + source;
      if (one_cell_) sub = s.rain_later && s.minutes > 0 && !forecast_.stale ? tessera::format(plugin_->text("rain_in"), s.minutes) : source;
      ui::set_text(sub_, sub);
    }
    const uint32_t cursor = map_ && newest_ ? frame_time(newest_, frame_) : 0;
    graph_.paint(plugin_, forecast_, now_, cursor);
    if (map_) map_view_.show(plugin_, newest_, frame_, now_);
  }

  Buienradar *plugin_;
  int tile_ = -1;
  char name_[48] = {};
  float latitude_ = 52.1f, longitude_ = 5.18f;
  int pace_ = 2;
  Play play_ = Play::ALL;
  bool map_ = false, one_cell_ = false;
  int frame_ = 0;
  uint32_t now_ = 0, newest_ = 0, minute_ = 0, moved_ = 0;
  Forecast forecast_;
  Graph graph_;
  Map map_view_;
  lv_obj_t *head_ = nullptr, *sub_ = nullptr;
};

// ---- The card ----

// Over the whole glass: the map at the left with the moment, its rain and the keys beside it, the graph across the
// bottom as the timeline. A tap on the graph shows that moment on the map; a tap on the map plays or stops it.
class RadarCard : public tessera::Card {
 public:
  explicit RadarCard(Buienradar *plugin) : plugin_(plugin) {}

  void open(const tessera::CardContext &c) override {
    const Handoff &from = plugin_->handoff;
    frame_ = std::max(FIRST_FRAME, std::min(LAST_FRAME, from.frame));
    pace_ = from.pace;
    const int w = c.width, h = c.height, gap = ui::px(10);
    const int body = ui::line_height(Font::BODY), value = ui::line_height(Font::VALUE);
    // Wide: the map at the left over the whole height, the moment, the keys and the graph beside it. Square: the map
    // large at the top left, the moment and the keys stacked beside it, the words and a lower graph under both. Tall:
    // the map on top over the whole width, the rest under it. The graph takes what the words leave.
    int x = 0, y = 0;
    if (ui::pictures() && w * 100 < h * WIDE && w * 100 >= h * SQUARE) {
      has_map_ = true;
      open_square(c, w, h, gap, body, value);
      return;
    }
    if (ui::pictures()) {
      has_map_ = true;
      if (w * 100 >= h * WIDE) {
        const int mw = Map::fit_width(w * 58 / 100, h);
        map_view_.make(c.parent, 0, (h - Map::height_of(mw)) / 2, mw, from.latitude, from.longitude);
        x = mw + gap * 2;
      } else {
        // The map takes what the moment, the words and a graph of at least a third of the room leave.
        const int key = std::max(ui::mm(9), ui::line_height(Font::ICON) + ui::px(8));
        const int words = std::max(value + body, key) + ui::px(6) + 2 * body + gap;
        const int graph = std::max(h * 28 / 100, 4 * body);
        const int mw = Map::fit_width(w, std::max(2, h - words - graph - gap));
        map_view_.make(c.parent, (w - mw) / 2, 0, mw, from.latitude, from.longitude);
        y = map_view_.height() + gap;
      }
    }
    const int cw = std::max(1, w - x);
    if (has_map_) {
      // The moment at the left, the three keys beside it at the right: back a frame, play or stop, on a frame.
      key_ = std::min(std::max(ui::mm(9), ui::line_height(Font::ICON) + ui::px(8)), (cw - 2 * ui::px(8)) / 4);
      const int keys_w = 3 * key_ + 2 * ui::px(8);
      keys_x_ = x + cw - keys_w;
      keys_y_ = y + std::max(0, (value + body - key_) / 2);
      const int time_w = std::max(1, cw - keys_w - gap);
      time_ = ui::label(c.parent, Font::VALUE, theme::INK);
      lv_obj_set_width(time_, time_w);
      lv_obj_set_pos(time_, x, y);
      when_ = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_set_width(when_, time_w);
      lv_obj_set_pos(when_, x, y + value);
      y += std::max(value + body, key_) + ui::px(6);
      for (int k = 0; k < 3; ++k) {
        keys_[k] = ui::block(c.parent, theme::BUTTON);
        lv_obj_set_size(keys_[k], key_, key_);
        lv_obj_set_style_radius(keys_[k], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_pos(keys_[k], keys_x_ + k * (key_ + ui::px(8)), keys_y_);
        icons_[k] = ui::label(keys_[k], Font::ICON, theme::INK);
        lv_obj_set_width(icons_[k], key_);
        lv_obj_set_style_text_align(icons_[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(icons_[k], 0, (key_ - ui::line_height(Font::ICON)) / 2);
      }
      ui::set_text(icons_[0], ui::icon(ICON_PREVIOUS));
      ui::set_text(icons_[2], ui::icon(ICON_NEXT));
    }
    head_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(head_, cw, 2 * body);
    lv_obj_set_pos(head_, x, y);
    y += 2 * body + gap;
    graph_.make(c.parent, x, y, cw, std::max(3 * body, h - y), true);
  }

  // The square card: the map, a column beside it with the moment and the three keys in a row under it, the words and
  // the graph across the bottom.
  void open_square(const tessera::CardContext &c, int w, int h, int gap, int body, int value) {
    const Handoff &from = plugin_->handoff;
    const int graph = std::max(h * 24 / 100, 3 * body + ui::px(8));
    const int top = h - graph - 2 * body - 2 * gap;
    const int column = std::max({ui::text_width("00:00", Font::VALUE) + ui::px(8), 3 * ui::mm(7) + 2 * ui::px(8),
                                 w * 28 / 100});
    const int mw = Map::fit_width(w - column - gap * 2, top), mh = Map::height_of(mw);
    map_view_.make(c.parent, 0, 0, mw, from.latitude, from.longitude);
    const int x = mw + gap * 2, cw = std::max(1, w - x);
    time_ = ui::label(c.parent, Font::VALUE, theme::INK);
    lv_obj_set_width(time_, cw);
    lv_obj_set_pos(time_, x, 0);
    when_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(when_, cw, 2 * body);
    lv_obj_set_pos(when_, x, value);
    // The keys in a row under the moment, as large as the column allows.
    key_ = std::max(ui::px(24), std::min(std::max(ui::mm(9), ui::line_height(Font::ICON) + ui::px(8)),
                                         (cw - 2 * ui::px(8)) / 3));
    keys_x_ = x;
    keys_y_ = value + 2 * body + gap;
    for (int k = 0; k < 3; ++k) {
      keys_[k] = ui::block(c.parent, theme::BUTTON);
      lv_obj_set_size(keys_[k], key_, key_);
      lv_obj_set_style_radius(keys_[k], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_pos(keys_[k], keys_x_ + k * (key_ + ui::px(8)), keys_y_);
      icons_[k] = ui::label(keys_[k], Font::ICON, theme::INK);
      lv_obj_set_width(icons_[k], key_);
      lv_obj_set_style_text_align(icons_[k], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(icons_[k], 0, (key_ - ui::line_height(Font::ICON)) / 2);
    }
    ui::set_text(icons_[0], ui::icon(ICON_PREVIOUS));
    ui::set_text(icons_[2], ui::icon(ICON_NEXT));
    const int y = std::max(mh, keys_y_ + key_) + gap;
    head_ = ui::text(c.parent, Font::BODY, theme::MUTED);
    lv_obj_set_size(head_, w, 2 * body);
    lv_obj_set_pos(head_, 0, y);
    graph_.make(c.parent, 0, y + 2 * body + gap, w, std::max(3 * body, h - y - 2 * body - gap), true);
  }

  // Which of the three keys the finger is on: 0 back, 1 play or stop, 2 on; -1 for none.
  int key_at(const tessera::Point &p) const {
    const int along = p.x - keys_x_, across = p.y - keys_y_;
    if (along < 0 || across < 0 || across >= key_) return -1;
    const int k = along / (key_ + ui::px(8));
    return k <= 2 && along - k * (key_ + ui::px(8)) < key_ ? k : -1;
  }

  void on_state(JsonObjectConst data) override {
    forecast_.read(data);
    paint();
  }

  void on_tick(uint32_t epoch) override {
    now_ = epoch;
    bool changed = false;
    const uint32_t newest = newest_frame(epoch);
    if (newest != newest_) {
      newest_ = newest;
      changed = true;
    }
    if (playing_ && newest_) {
      const uint32_t ms = esphome::millis();
      if (ms - moved_ >= static_cast<uint32_t>(pace_) * 1000u) {
        frame_ = frame_ >= LAST_FRAME ? FIRST_FRAME : frame_ + 1;
        moved_ = ms;
        changed = true;
      }
    }
    if (epoch / 60 != minute_) {
      minute_ = epoch / 60;
      changed = true;
    }
    if (changed) paint();
  }

  void on_theme() override {
    if (time_) ui::set_color(time_, theme::INK);
    for (auto *l : {when_, head_}) if (l) ui::set_color(l, theme::MUTED);
    for (int k = 0; k < 3; ++k) {
      if (keys_[k]) ui::set_fill(keys_[k], Tone::NORMAL, theme::BUTTON);
      if (icons_[k]) ui::set_color(icons_[k], theme::INK);
    }
    graph_.colors();
    if (has_map_) map_view_.colors();
  }

  void on_tap() override {
    if (!has_map_ || !newest_) return;
    const tessera::Point p = tessera::tap_point();
    if (p.x < 0) return;
    if (graph_.hit(p)) {
      // The moment under the finger, as the nearest frame.
      const int64_t d = static_cast<int64_t>(graph_.time_at(p.x)) - static_cast<int64_t>(newest_);
      frame_ = static_cast<int>(std::max<int64_t>(FIRST_FRAME, std::min<int64_t>(LAST_FRAME, (d + (d >= 0 ? 150 : -150)) / 300)));
      playing_ = false;
    } else if (const int k = key_at(p); k >= 0) {
      if (k == 0) {
        frame_ = std::max(FIRST_FRAME, frame_ - 1);
        playing_ = false;
      } else if (k == 1) {
        playing_ = !playing_;
        moved_ = esphome::millis();
      } else if (k == 2) {
        frame_ = std::min(LAST_FRAME, frame_ + 1);
        playing_ = false;
      }
    } else if (map_view_.hit(p)) {
      playing_ = !playing_;
      moved_ = esphome::millis();
    } else {
      return;
    }
    paint();
  }

 private:
  void paint() {
    const Summary s = summarise(plugin_, forecast_, now_);
    ui::set_text(head_, (s.sub.empty() ? s.head : s.head + ". " + s.sub) + " · " + plugin_->text("source"));
    uint32_t cursor = 0;
    if (has_map_ && newest_) {
      cursor = frame_time(newest_, frame_);
      ui::set_text(time_, tessera::clock_text(cursor));
      // The moment against now, and the rain then at the place: "+15 min · 0.4 mm/h".
      std::string when = relative(plugin_, cursor, now_);
      if (forecast_.covers(cursor)) {
        const float mm = forecast_.mm[forecast_.index(cursor)];
        when += std::string(" · ") + (mm >= RAIN ? tessera::fill(plugin_->text("mm"), "mm", millimetres(mm))
                                                 : std::string(plugin_->text("dry")));
      }
      ui::set_text(when_, when);
      ui::set_text(icons_[1], ui::icon(playing_ ? ICON_PAUSE : ICON_PLAY));
      map_view_.show(plugin_, newest_, frame_, now_);
    }
    graph_.paint(plugin_, forecast_, now_, cursor);
  }

  Buienradar *plugin_;
  int frame_ = 0, pace_ = 2, key_ = 1, keys_x_ = 0, keys_y_ = 0;
  bool has_map_ = false, playing_ = false;
  uint32_t now_ = 0, newest_ = 0, minute_ = 0, moved_ = 0;
  Forecast forecast_;
  Graph graph_;
  Map map_view_;
  lv_obj_t *time_ = nullptr, *when_ = nullptr, *head_ = nullptr;
  lv_obj_t *keys_[3] = {}, *icons_[3] = {};
};

void Buienradar::setup() {
  add_tile("rain", [this]() { return new RainTile(this); });
  add_card("radar", [this]() { return new RadarCard(this); }, true);
}

}  // namespace esphome::buienradar
