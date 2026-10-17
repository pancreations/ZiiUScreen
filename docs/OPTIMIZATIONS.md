# Performance research

Video timing, encoder load, capture timing, and radio completion all affect
GamePad stability. Measure one change at a time, preserve a known working local
build, and keep captures and measurements under `.local/`. Do not infer hardware
success from host tests. Private tuning sessions are excluded from this snapshot.

The stream currently uses the source-built encoder. `-PreFeatures` refers to a
private historical binary that is not distributed. CPU presets and priority
changes may affect other applications. Reconnect and latency work is unfinished.
