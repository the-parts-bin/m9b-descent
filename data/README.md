# Descent's data

The shareware 1.4 data, freely distributable:

| File | Size | MD5 |
|---|---|---|
| `descent.hog` | 2,339,773 | `cf1caf9c7a0681de80fac8cb593b33eb` |
| `descent.pig` | 5,092,871 | `f8dc0f2b3ee8baa2c622f57b470fe147` |

Nothing to do by hand: when they are missing, `tools/get_data.sh` (which
the SD card build in m9b-system runs) downloads `desc14sw.zip` from
classicdosgames.com and extracts them here. To use copies you already
have, put them here first. The port is built for the shareware data
(`-DSHAREWARE`); the full game's hasn't been tried.

The music is rendered from `descent.hog` (`tools/make_music.sh`).
