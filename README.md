# xtide-nearest

Find the nearest XTide station (tide and/or current) to a GPS point and emit prediction CSV to stdout.

This tool:
- Reads one or more XTide harmonics `.tcd` databases via **libtcd**
- Finds the nearest **tide** station (height) and/or nearest **current** station (speed+direction)
- Runs XTide's `tide` command in **raw CSV** mode to generate time series

## Requirements

### Runtime
- `tide` executable from XTide must be installed and on PATH.

### Build-time
- libtcd headers + library (often packaged with XTide dev packages)
- CMake 3.20+
- C++20 compiler

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
````

## Usage

### Nearest TIDE station

```bash
./xtide-nearest \
  --lat 40.7000 --lon -74.0120 \
  --tcd "harmonics-dwf-20251228/harmonics-dwf-20251228-free.tcd" \
  nearest --no-current --top 1
```

### Nearest CURRENT station

```bash
./xtide-nearest \
  --lat 40.7000 --lon -74.0120 \
  --tcd "harmonics-dwf-20251228/harmonics-dwf-20251228-free.tcd" \
  nearest --no-tide --top 1
```

### Currents only

```bash
./xtide-nearest \
  --lat 40.7000 --lon -74.0120 \
  --tcd "harmonics-dwf-20251228/harmonics-dwf-20251228-free.tcd" \
  predict --kind current \
  --begin "2026-01-22 00:00" \
  --end   "2026-01-23 00:00" \
  --step  "00:10" 
```

### Tide only 

```bash
./xtide-nearest \
  --lat 40.7000 --lon -74.0120 \
  --tcd "harmonics-dwf-20251228/harmonics-dwf-20251228-free.tcd" \
  predict --kind tide \
  --begin "2026-01-22 00:00" \
  --end   "2026-01-23 00:00" \
  --step  "00:10" 
```

