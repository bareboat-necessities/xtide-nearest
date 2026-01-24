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

JSON Output:

```
$ ./xtide-nearest   --lat 40.7000 --lon -74.0120 --json  --tcd "harmonics-dwf-20251228/harmonics-dwf-20251228-free.tcd"   predict --kind current   --begin "2026-01-22 00:00"   --end   "2026-01-22 00:30"   --step  "00:10"
{"ok":true,"command":"predict","lat":40.700000000000003,"lon":-74.012,"kind":"current","fingerprint":"2aaadfd3607231e3","cache":{"enabled":true,"path":"C:\\Users\\17326\\AppData\\Local\\xtide-nearest\\nearest.cache","loaded":true,"hit":true},"station":{"name":"Dimond Reef (depth 11 ft), Upper Bay, New York Harbor, New York Current","lat_deg":40.697899999999997,"lon_deg":-74.021299999999997,"kind":"current","units":"knots","min_dir_deg":137,"max_dir_deg":10,"tcd_path":"C:\\Users\\17326\\Downloads\\windows-x64(16)\\xtide-nearest-sha-5f43094-windows-x64\\harmonics-dwf-20251228\\harmonics-dwf-20251228-free.tcd"},"distance_km":0.818045,"tide":{"bin":"tide","begin":"2026-01-22 00:00","end":"2026-01-22 00:30","step":"00:10","mode":"r","format":"c","utc":false,"include_sunmoon":false,"omit_units":false},"runner_rc":0,"output":{"format":"tide_stdout","text":"Dimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769058000,0.445081\r\nDimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769058600,0.379907\r\nDimond Reef (depth 11 ft)| Upper Bay| New York Harbor| New York Current,1769059200,0.328561\r\n"}}
```

## Cached lookup of Stations

In xtide-nearest/nearest.cache

- Linux: ~/.cache/xtide-nearest/nearest.cache (or $XDG_CACHE_HOME/...)
- Windows: %LOCALAPPDATA%\xtide-nearest\nearest.cache

```
# xtide-nearest cache (key=value, percent-encoded)
current_lat=40.697899999999997
current_lon=-74.021299999999997
current_max_dir=10
current_min_dir=137
current_name=Dimond Reef (depth 11 ft), Upper Bay, New York Harbor, New York Current
current_tcd=C:\Users\17326\Downloads\windows-x64(15)\xtide-nearest-sha-f14439a-windows-x64\harmonics-dwf-20251228\harmonics-dwf-20251228-free.tcd
current_units=knots
epoch_s=1769195326
fingerprint=84c8bb69110bf5d7
has_current=1
has_tide=1
lat=40.700000000000003
lon=-74.012
tide_lat=40.70055
tide_lon=-74.014169999999993
tide_max_dir=361
tide_min_dir=361
tide_name=The Battery, New York Harbor, New York
tide_tcd=C:\Users\17326\Downloads\windows-x64(15)\xtide-nearest-sha-f14439a-windows-x64\harmonics-dwf-20251228\harmonics-dwf-20251228-free.tcd
tide_units=feet
version=1
```

## Optional config

- Linux: ~/.config/xtide-nearest/tcd.conf (or $XDG_CONFIG_HOME/xtide-nearest/tcd.conf)
- Windows: %APPDATA%\xtide-nearest\tcd.conf

Sample tdc.conf:

```
# xtide-nearest TCD search paths

/etc/tcdata
/usr/share/tcdata
/usr/share/opencpn/tcdata
/usr/local/share/opencpn/tcdata

# You can also pin an exact file:
#/home/user/.tcdata/harmonics.tcd
```


## Debian packages install via apt

Add the repo.

Create /etc/apt/sources.list.d/xtide-nearest.list:

```bash
sudo tee /etc/apt/sources.list.d/xtide-nearest.list >/dev/null <<'EOF'
deb [trusted=yes] https://github.com/bareboat-necessities/xtide-nearest/releases/download/apt/ ./
EOF
```

Update + install

```bash
sudo apt-get update
sudo apt-get install xtide-nearest
```