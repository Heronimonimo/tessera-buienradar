# buienradar: a plugin for Tessera

This repository is one plugin for Tessera, the ESP32 touch screens for Home Assistant. Its rules, its docs and its
tools live in the plugins repository: clone it beside this one, then read its AGENTS.md before you change anything.

```sh
git clone https://github.com/MaxGramser/tessera-plugins ../tessera-plugins
```

- The rules and the order to read the docs in: `../tessera-plugins/AGENTS.md`.
- Check the plugin, after every change: `python3 ../tessera-plugins/tools/check.py .` (must print `buienradar: ok`).
- See what it draws on every kind of screen, after every change to its C++, its manifest or `preview.yaml`:
  `python3 ../tessera-plugins/tools/preview.py . --strict` (`--setup` once first; `../tessera-plugins/docs/PREVIEW.md`).
  The first build of each screen takes minutes. Read the findings it prints, `.preview/summary.json` and the pictures in
  `.preview/`, fix what it says, and run it again until it ends without a warning.
- Then make its picture on the Plugins page: `python3 ../tessera-plugins/tools/store_image.py .` writes `store.png`
  (`../tessera-plugins/docs/PUBLISHING.md`, "The picture on the Plugins page"). Look at it, commit it, and it reaches
  the app with your next release.
