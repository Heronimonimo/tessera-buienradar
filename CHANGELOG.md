# Changelog

## 0.3.6 - 2026-10-23
- The time axis shows whole hours (21:00, 22:00) at their place on the graph; the line at now stays.

## 0.3.5 - 2026-10-22
- Every tile's graph shows its times (now and the hours) under the bars, also on a low tile.

## 0.3.4 - 2026-10-21
- The rain on the map no longer goes missing when Buienradar runs late: the map takes its newest radar frame from
  Buienradar's forecast for your place (where its measured past ends) instead of guessing it from the clock. That
  frame can be over half an hour old while Buienradar catches up; the badge on the map says how old.

## 0.3.3 - 2026-10-21
- The graph is linear now: its top is 1, 2, 5, 10 mm/h or more, whatever the heaviest rain of the forecast fits
  under, so light rain still shows.
- The card's graph has a scale: lines at 0.5, 2.5 and 10 mm/h with their numbers, the kinds of rain between them
  (drizzle, light, moderate, heavy rain), and the unit above them. The card names the kind of rain of its moment.
- The lines between the kinds of rain are dotted.
- No tile or card ends its line in "Buienradar" any more.
- The card is titled with the tile's name, as Tessera's own cards are.

## 0.3.2 - 2026-10-21
- When Buienradar does not answer (a few minutes without internet), the tile keeps its last forecast, says it is
  older, and the map keeps playing the frames it already has instead of going blank.

## 0.3.1 - 2026-10-21
- "Buienradar does not answer" is gone for a place on a whole degree (52.0, 5.0): Buienradar only answers a
  coordinate written in its shortest form, and the list now sends it so. Choose the place again if the tile says it.
- Latitude starts at 49.6: Buienradar has no forecast at 49.5.

## 0.3.0 - 2026-10-21
- Latitude and longitude are now chosen from a list in steps of 0.1, always shown with one decimal. Choose your
  place again after this update: the tile starts at De Bilt (52.1, 5.2).
- A tile one row high shows the words at the left and the graph beside them.
- The words beside a graph or a map take only the lines they need.

## 0.2.2 - 2026-10-21
- The card plays the map as soon as it opens; the key in the middle stops it.
- Latitude and longitude step by 0.1 in the inspector.
- A 1x1 tile sets its words in the middle, each over the lines it needs.

## 0.2.1 - 2026-10-21
- The map no longer goes blank for a while every five minutes: the forecast frames come from a Buienradar forecast
  that is always published, not one that sometimes was not there yet.
- A 1x1 tile shows the words alone, larger and in the middle; the graph did not fit beside them.

## 0.2.0 - 2026-10-21
- A square tile or card shows a larger map with the words, or the time and the keys, beside it, and a lower graph
  under both.
- A 2x2 tile of at least 35 mm a side shows the map too.
- The card's back, play and next keys stay in a row.

## 0.1.1 - 2026-10-21
- The map is Buienradar's own radar background, the very area of the rain, so rain and map line up and the map
  has no edge where the radar stops.
- A wide tile or card shows the map beside the graph, a square or tall one the map above it, with less empty room.
- The card says the rain of the moment under its time.

## 0.1.0 - 2026-10-21
- First version: Buienradar's rain graph for a place on every tile size, the radar map that plays by itself on tiles
  of 3x2 or 2x3 and larger, and a card with both where the graph picks the map's moment.
