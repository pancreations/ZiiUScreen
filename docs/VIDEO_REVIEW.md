# Video validation limits

The offline `tools/ziiu-drc/check_stream.py` checks packet/video behavior using
ffmpeg/ffprobe and numpy. Capture and motion analysis scripts can create recordings
of the desktop: select the intended display and keep their output private.

The x264 patch preserves DRH chunk state across VBV row retries and defers slice
callbacks until frame finalization. It targets the revision pinned by the build
script. Offline decoder results do not establish live GamePad stability, timing,
or compatibility. Private recordings and session logs are excluded.
