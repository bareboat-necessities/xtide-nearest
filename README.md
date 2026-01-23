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

### Getting tides and currents files 

Example

```bash
wget https://flaterco.com/files/xtide/harmonics-dwf-20251228-free.tar.xz
xz -cd harmonics-dwf-20251228-free.tar.xz | tar xvf -
```

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

Output:
```
# kind=current station=Dimond Reef (depth 11 ft), Upper Bay, New York Harbor, New York Current distance_km=0.818 begin=2026-01-22 00:00 end=2026-01-23 00:00 step=00:10 utc=false
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769058000,0.445081
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769058600,0.379907
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769059200,0.328561
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769059800,0.291488
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769060400,0.268112
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769061000,0.256929
Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769061600,0.255676
...
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

Output:
```
# kind=tide station=The Battery, New York Harbor, New York distance_km=0.193 begin=2026-01-22 00:00 end=2026-01-23 00:00 step=00:10 utc=false
The Battery| New York Harbor| New York,1769058000,3.281012
The Battery| New York Harbor| New York,1769058600,3.131090
The Battery| New York Harbor| New York,1769059200,2.970155
The Battery| New York Harbor| New York,1769059800,2.799378
The Battery| New York Harbor| New York,1769060400,2.620230
The Battery| New York Harbor| New York,1769061000,2.434448
...
```