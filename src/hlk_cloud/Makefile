#
# Copyright (C) 2011-2017 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=hi_link
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_INSTALL:=1
PKG_BUILD_PARALLEL:=1

PKG_BUILD_DIR:=$(BUILD_DIR)/hi_link
VERSION_BASE_VAL := V1.0.38
OUTPUT_HEADER_FILE_PATH := $(PKG_BUILD_DIR)/source/hi_cfm_version.h

include $(INCLUDE_DIR)/package.mk

define Package/hi_link
  SECTION:=HLK Custom
  CATEGORY:=HLK Custom
  TITLE:=hilink private cloud Library
  DEPENDS:= +libcurl
  MENU:=1
endef

define Package/hi_link/description
  This lib is a library of HiLink private cloud
endef

define Package/hi_link/config
	source "$(SOURCE)/Config.in"
endef

ifdef CONFIG_HLK_PRODUCT_ID
  PRODUCT_ID_CLEANED := $(CONFIG_HLK_PRODUCT_ID)
else
  PRODUCT_ID_CLEANED := RM65
endif
_DATETIME_STR=$(shell date +'%y%m%d.%H%M%S')

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR); \
	cp -rfd ./src/* $(PKG_BUILD_DIR)/; \
	mkdir -p $(dir $(OUTPUT_HEADER_FILE_PATH)); \
	echo "#define AT_VERSION \"$(PRODUCT_ID_CLEANED)-$(VERSION_BASE_VAL)-$(_DATETIME_STR)\"" > "$(OUTPUT_HEADER_FILE_PATH)"; \
	echo "Generated $(OUTPUT_HEADER_FILE_PATH) with AT_VERSION=$(PRODUCT_ID_CLEANED)-$(VERSION_BASE_VAL)-$(_DATETIME_STR)"
endef

MAKE_FLAGS += \
	      	CONFIG_SUPPORT_OPENWRT=y \
			CONFIG_HLK_PRODUCT=$(PRODUCT_ID_CLEANED)
			
define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/lib $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_BUILD_DIR)/usr/lib/libhi_link.so $(1)/usr/lib/
endef


define Package/hi_link/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/usr/lib/libhi_link.so $(1)/usr/lib/
endef


$(eval $(call BuildPackage,hi_link))
