#!/usr/bin/env bash
set -euo pipefail

OPENWRT_VERSION="${OPENWRT_VERSION:-24.10.6}"
OPENWRT_TARGET="${OPENWRT_TARGET:?OPENWRT_TARGET must be set, e.g. x86/64}"
OPENWRT_JOBS="${OPENWRT_JOBS:-2}"
OPENWRT_PACKAGE_VERSION="${OPENWRT_PACKAGE_VERSION:-0.0.0~local}"
OPENWRT_PACKAGE_RELEASE="${OPENWRT_PACKAGE_RELEASE:-1}"
LIBTCD_URL="${LIBTCD_URL:-https://flaterco.com/files/xtide/libtcd-2.2.7-r3.tar.xz}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${ROOT_DIR}/.openwrt-sdk"
DIST_DIR="${ROOT_DIR}/dist/openwrt/${OPENWRT_TARGET//\//-}"
SDK_DIR=""

rm -rf "${WORK_DIR}" "${DIST_DIR}"
mkdir -p "${WORK_DIR}" "${DIST_DIR}"

SDK_BASE_URL="https://downloads.openwrt.org/releases/${OPENWRT_VERSION}/targets/${OPENWRT_TARGET}"
SDK_LISTING="${WORK_DIR}/sdk-listing.html"

echo "Downloading OpenWrt SDK listing from ${SDK_BASE_URL}/"
curl -fsSL --retry 3 --retry-delay 2 "${SDK_BASE_URL}/" -o "${SDK_LISTING}"
SDK_ARCHIVE="$(sed -n 's/.*href="\([^"]*openwrt-sdk-[^"]*[Ll]inux-x86_64\.tar\.zst\)".*/\1/p' "${SDK_LISTING}" | head -n1)"
if [[ -z "${SDK_ARCHIVE}" ]]; then
  echo "ERROR: Could not find an OpenWrt SDK archive for ${OPENWRT_VERSION} ${OPENWRT_TARGET}" >&2
  exit 1
fi

echo "Downloading ${SDK_ARCHIVE}"
curl -fL --retry 3 --retry-delay 2 "${SDK_BASE_URL}/${SDK_ARCHIVE}" -o "${WORK_DIR}/${SDK_ARCHIVE}"
tar --zstd -xf "${WORK_DIR}/${SDK_ARCHIVE}" -C "${WORK_DIR}"
SDK_DIR="$(find "${WORK_DIR}" -maxdepth 1 -type d -name 'openwrt-sdk-*' | head -n1)"
if [[ -z "${SDK_DIR}" ]]; then
  echo "ERROR: SDK extraction did not create an openwrt-sdk-* directory" >&2
  exit 1
fi

PKG_DIR="${SDK_DIR}/package/xtide-nearest"
mkdir -p "${PKG_DIR}/src"
rsync -a --delete \
  --exclude '.git' \
  --exclude '.github' \
  --exclude '.openwrt-sdk' \
  --exclude 'build' \
  --exclude 'dist' \
  "${ROOT_DIR}/" "${PKG_DIR}/src/"

cat > "${PKG_DIR}/Makefile" <<'MAKEFILE'
include $(TOPDIR)/rules.mk

PKG_NAME:=xtide-nearest
PKG_VERSION:=$(OPENWRT_PACKAGE_VERSION)
PKG_RELEASE:=$(OPENWRT_PACKAGE_RELEASE)
PKG_LICENSE:=GPL-3.0-or-later
PKG_MAINTAINER:=$(shell git config --get remote.origin.url 2>/dev/null || echo xtide-nearest)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

LIBTCD_URL:=$(if $(LIBTCD_URL),$(LIBTCD_URL),https://flaterco.com/files/xtide/libtcd-2.2.7-r3.tar.xz)
LIBTCD_BUILD_DIR:=$(PKG_BUILD_DIR)/libtcd
LIBTCD_PREFIX:=$(PKG_BUILD_DIR)/libtcd-prefix

CMAKE_OPTIONS += \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_PREFIX_PATH=$(LIBTCD_PREFIX) \
	-DTCD_INCLUDE_DIR:PATH=$(LIBTCD_PREFIX)/include \
	-DTCD_LIBRARY:FILEPATH=$(LIBTCD_PREFIX)/lib/libtcd.so

TARGET_LDFLAGS += -Wl,-rpath,/usr/lib/xtide-nearest/lib

define Package/xtide-nearest
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Find nearest XTide tide/current station
  DEPENDS:=+libstdcpp +libgcc
endef

define Package/xtide-nearest/description
  Command-line tool that finds the nearest tide or current station from an
  XTide harmonics database.
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
endef

define Build/Compile/libtcd
	rm -rf $(LIBTCD_BUILD_DIR) $(LIBTCD_PREFIX) $(PKG_BUILD_DIR)/libtcd.tar.xz
	curl -fL --retry 3 --retry-delay 2 $(LIBTCD_URL) -o $(PKG_BUILD_DIR)/libtcd.tar.xz
	mkdir -p $(LIBTCD_BUILD_DIR)
	tar -xf $(PKG_BUILD_DIR)/libtcd.tar.xz -C $(LIBTCD_BUILD_DIR) --strip-components=1
	( cd $(LIBTCD_BUILD_DIR); \
		CC="$(TARGET_CC)" \
		CXX="$(TARGET_CXX)" \
		AR="$(TARGET_AR)" \
		RANLIB="$(TARGET_RANLIB)" \
		CFLAGS="$(TARGET_CFLAGS) $(FPIC)" \
		CXXFLAGS="$(TARGET_CXXFLAGS) $(FPIC)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		./configure --host=$(GNU_TARGET_NAME) --build=$(GNU_HOST_NAME) --prefix=$(LIBTCD_PREFIX) --disable-static; \
		$(MAKE); \
		$(MAKE) install; \
	)
endef

define Build/Configure
	$(call Build/Compile/libtcd)
	$(call Build/Configure/Default)
endef

define Build/Compile
	$(call Build/Compile/Default)
endef

define Package/xtide-nearest/install
	$(INSTALL_DIR) $(1)/usr/lib/xtide-nearest/lib $(1)/usr/bin $(1)/usr/share/doc/xtide-nearest
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/xtide-nearest $(1)/usr/lib/xtide-nearest/xtide-nearest
	$(CP) $(LIBTCD_PREFIX)/lib/libtcd.so* $(1)/usr/lib/xtide-nearest/lib/
	$(INSTALL_BIN) ./files/xtide-nearest.wrapper $(1)/usr/bin/xtide-nearest
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/README.md $(1)/usr/share/doc/xtide-nearest/README.md
endef

$(eval $(call BuildPackage,xtide-nearest))
MAKEFILE

mkdir -p "${PKG_DIR}/files"
cat > "${PKG_DIR}/files/xtide-nearest.wrapper" <<'EOF_WRAPPER'
#!/bin/sh
set -e
exec /usr/lib/xtide-nearest/xtide-nearest "$@"
EOF_WRAPPER
chmod 0755 "${PKG_DIR}/files/xtide-nearest.wrapper"

make -C "${SDK_DIR}" defconfig
make -C "${SDK_DIR}" "package/xtide-nearest/compile" -j"${OPENWRT_JOBS}" V=s
find "${SDK_DIR}/bin/packages" -type f -name 'xtide-nearest_*.ipk' -exec cp -v {} "${DIST_DIR}/" \;

echo "OpenWrt packages written to ${DIST_DIR}"
