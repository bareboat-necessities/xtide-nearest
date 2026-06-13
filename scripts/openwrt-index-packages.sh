#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR="${1:?usage: openwrt-index-packages.sh INPUT_DIR OUTPUT_DIR [flat]}"
OUTPUT_DIR="${2:?usage: openwrt-index-packages.sh INPUT_DIR OUTPUT_DIR [flat]}"
MODE="${3:-flat}"

rm -rf "${OUTPUT_DIR}"
mkdir -p "${OUTPUT_DIR}"

if [[ "${MODE}" != "flat" ]]; then
  echo "ERROR: only flat mode is currently supported" >&2
  exit 1
fi

extract_control() {
  local ipk="$1"
  local tmpdir="$2"

  if ar t "${ipk}" >/dev/null 2>&1; then
    ar p "${ipk}" control.tar.gz | tar -xz -C "${tmpdir}" ./control
  else
    tar -xOf "${ipk}" ./control.tar.gz 2>/dev/null | tar -xz -C "${tmpdir}" ./control
  fi
}

find "${INPUT_DIR}" -type f -name '*.ipk' -print0 | while IFS= read -r -d '' ipk; do
  cp -v "${ipk}" "${OUTPUT_DIR}/"
done

packages_file="${OUTPUT_DIR}/Packages"
: > "${packages_file}"
for ipk in "${OUTPUT_DIR}"/*.ipk; do
  [[ -e "${ipk}" ]] || continue
  tmpdir="$(mktemp -d)"
  extract_control "${ipk}" "${tmpdir}"
  cat "${tmpdir}/control" >> "${packages_file}"
  printf 'Filename: %s\n' "$(basename "${ipk}")" >> "${packages_file}"
  printf 'Size: %s\n' "$(stat -c '%s' "${ipk}")" >> "${packages_file}"
  printf 'SHA256sum: %s\n\n' "$(sha256sum "${ipk}" | awk '{print $1}')" >> "${packages_file}"
  rm -rf "${tmpdir}"
done

gzip -9c "${packages_file}" > "${packages_file}.gz"
echo "OpenWrt package index written to ${OUTPUT_DIR}"
