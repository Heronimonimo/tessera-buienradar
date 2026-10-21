#!/usr/bin/env python3
"""Give the bench Buienradar's real pictures before a run.

    python3 tools/bench_pictures.py && python3 ../tessera-plugins/tools/preview.py .

The bench answers a place's picture only for an address preview.yaml names. The map has a fixed address;
the radar frames' addresses follow the clock (components/buienradar/buienradar.cpp, frame_url), so this writes the
`pictures` of preview.yaml anew: the map from preview/, and every radar frame the screen may ask for in the next
half hour as preview/radar.png, one real frame of Buienradar's. Run it again when a bench run is more than half an hour
later.
"""
import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MARK = '# pictures: written by tools/bench_pictures.py'
STEP, LAG, RUN_BEHIND = 300, 600, 2          # as buienradar.cpp
FIRST, LAST = -2, 24 - RUN_BEHIND
CDN = 'https://processing-cdn.buienradar.nl/processing/nl/rain/'


def stamp(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y%m%d%H%M')


def frames(newest):
    for k in range(FIRST, LAST + 1):
        t = newest + k * STEP
        yield (CDN + 'current/webm/' + stamp(t) + '.png') if k <= 0 else \
              (CDN + 'forecast/runs/webm/' + stamp(newest - RUN_BEHIND * STEP) + '/' + stamp(t) + '.png')


def main():
    now = int(datetime.datetime.now(datetime.timezone.utc).timestamp())
    # The plugin takes the newest frame from preview.yaml's answer (its past ends 20 minutes before the run), or from
    # the clock (LAG): every newest frame of either within the next half hour.
    newest = (now - 20 * 60) // STEP * STEP
    lines = [MARK, 'pictures:']
    lines.append('  "https://cdn.buienradar.nl/resources/image-api-backgrounds/map-nl-1058x916-webmercator_full.jpg": '
                 'preview/map.jpg')
    urls = []
    for run in range(newest, (now - LAG) // STEP * STEP + 7 * STEP, STEP):
        urls += [u for u in frames(run) if u not in urls]
    lines += [f'  "{u}": preview/radar.png' for u in urls]
    path = ROOT / 'preview.yaml'
    text = path.read_text(encoding='utf-8')
    head = text.split(MARK)[0].rstrip('\n')
    path.write_text(head + '\n\n' + '\n'.join(lines) + '\n', encoding='utf-8')
    print(f'preview.yaml: the map and {len(urls)} radar frames, for a bench run in the next half hour')


if __name__ == '__main__':
    main()
