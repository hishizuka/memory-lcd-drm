obj-m += sharp-drm.o
sharp-drm-objs += src/main.o src/kms.o src/registry.o src/fb_update.o src/control.o \
	src/backlight.o src/render.o src/spi_io.o src/tx.o src/perf.o src/params.o

ifeq ($(CONFIG_ARM64),y)
ifeq ($(CONFIG_KERNEL_MODE_NEON),y)
sharp-drm-objs += src/render_neon.o
CFLAGS_src/render_neon.o += $(CC_FLAGS_FPU)
CFLAGS_REMOVE_src/render_neon.o += $(CC_FLAGS_NO_FPU)
endif
endif

.PHONY: all modules clean clean_modules install uninstall install_modules \
	install_aux install_dkms uninstall_dkms refresh_initramfs install_redraw_service \
	uninstall_redraw_service presenter install_presenter uninstall_presenter

# KERNELRELEASE is set by DKMS, can be different inside chroot
ifeq ($(KERNELRELEASE),)
KERNELRELEASE := $(shell uname -r)
endif
# LINUX_DIR is set by Buildroot
ifeq ($(LINUX_DIR),)
LINUX_DIR := /lib/modules/$(KERNELRELEASE)/build
endif

# Target platform selects the overlay source overlays/sharp-drm-$(PLATFORM).dts
# and where the built overlay is installed. The installed overlay is always
# named sharp-drm.dtbo so `dtoverlay=sharp-drm` works on every platform.
PLATFORM ?= rpi
DTS := overlays/sharp-drm-$(PLATFORM).dts
DTBO := $(DTS:.dts=.dtbo)
UDEV_RULE := udev/99-sharp-drm-params.rules
UDEV_RULE_DIR ?= /etc/udev/rules.d
REDRAW_SOURCE := scripts/mip_redraw.c
REDRAW_BINARY := scripts/mip-redraw
REDRAW_SERVICE := systemd/mip-redraw.service
REDRAW_INSTALL_DIR ?= /usr/local/libexec/sharp-drm
SYSTEMD_UNIT_DIR ?= /etc/systemd/system
USER_CC ?= cc
DKMS_NAME := sharp-drm
DKMS_VERSION := 2.0.0
DKMS_SOURCE_DIR := /usr/src/$(DKMS_NAME)-$(DKMS_VERSION)
DKMS_SOURCE_FILES := $(wildcard src/*.c) $(wildcard src/*.h)

ifneq ($(filter $(PLATFORM),rpi),)
# Raspberry Pi OS 12+ moved the firmware partition to /boot/firmware
ifeq ($(wildcard /boot/firmware/overlays),)
OVERLAY_DIR := /boot/overlays
else
OVERLAY_DIR := /boot/firmware/overlays
endif
else
$(error Unsupported PLATFORM '$(PLATFORM)' (supported: rpi))
endif

BOOT_CONFIG_LINE := dtoverlay=sharp-drm
BOOT_CMDLINE_ADD := console=tty2 fbcon=font:VGA8x8 fbcon=map:10

# Raspbian 12 moved config and cmdline to firmware
ifeq ($(wildcard /boot/firmware/config.txt),)
	CONFIG=/boot/config.txt
else
	CONFIG=/boot/firmware/config.txt
endif
ifeq ($(wildcard /boot/firmware/cmdline.txt),)
	CMDLINE=/boot/cmdline.txt
else
	CMDLINE=/boot/firmware/cmdline.txt
endif


all: $(DTBO) modules

modules:
	$(MAKE) -C '$(LINUX_DIR)' M='$(shell pwd)'

overlays/sharp-drm-%.dtbo: overlays/sharp-drm-%.dts
	dtc -@ -I dts -O dtb -W no-unit_address_vs_reg -o $@ $<

install_modules:
	$(MAKE) -C '$(LINUX_DIR)' M='$(shell pwd)' modules_install
	# Rebuild dependencies
	depmod -A
	$(MAKE) refresh_initramfs

refresh_initramfs:
	# Refresh initramfs when the current kernel boots from one.
	@if command -v update-initramfs >/dev/null 2>&1 \
		&& { [ -e /boot/initrd.img-$(KERNELRELEASE) ] \
			|| [ -e /boot/firmware/initrd.img-$(KERNELRELEASE) ]; }; then \
		update-initramfs -u -k '$(KERNELRELEASE)'; \
	fi

install: install_modules install_aux

install_dkms:
	@command -v dkms >/dev/null 2>&1 \
		|| { echo "dkms is required; install the dkms package first" >&2; exit 1; }
	@if dkms status -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)' 2>/dev/null \
		| grep -q .; then \
		dkms remove -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)' --all; \
	fi
	install -d -m 0755 '$(DKMS_SOURCE_DIR)/src'
	install -m 0644 Makefile dkms.conf '$(DKMS_SOURCE_DIR)/'
	install -m 0644 $(DKMS_SOURCE_FILES) '$(DKMS_SOURCE_DIR)/src/'
	dkms add -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)'
	dkms build -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)' \
		-k '$(KERNELRELEASE)'
	dkms install -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)' \
		-k '$(KERNELRELEASE)' --force
	$(MAKE) install_aux
	$(MAKE) refresh_initramfs

uninstall_dkms:
	-dkms remove -m '$(DKMS_NAME)' -v '$(DKMS_VERSION)' --all
	rm -f '$(DKMS_SOURCE_DIR)/Makefile' '$(DKMS_SOURCE_DIR)/dkms.conf'
	rm -f $(addprefix $(DKMS_SOURCE_DIR)/,$(DKMS_SOURCE_FILES))
	-rmdir '$(DKMS_SOURCE_DIR)/src' '$(DKMS_SOURCE_DIR)'
	$(MAKE) refresh_initramfs

$(REDRAW_BINARY): $(REDRAW_SOURCE)
	$(USER_CC) -O2 -Wall -Wextra -o $@ $<

install_redraw_service: $(REDRAW_BINARY)
	install -D -m 0755 $(REDRAW_BINARY) \
		$(REDRAW_INSTALL_DIR)/mip-redraw
	install -D -m 0644 $(REDRAW_SERVICE) \
		$(SYSTEMD_UNIT_DIR)/mip-redraw.service
	systemctl daemon-reload
	systemctl enable --now mip-redraw.service

uninstall_redraw_service:
	-systemctl disable --now mip-redraw.service
	rm -f $(SYSTEMD_UNIT_DIR)/mip-redraw.service
	rm -f $(REDRAW_INSTALL_DIR)/mip-redraw
	systemctl daemon-reload

presenter:
	$(MAKE) -C contrib/qml-dmabuf

install_presenter:
	$(MAKE) -C contrib/qml-dmabuf install

uninstall_presenter:
	$(MAKE) -C contrib/qml-dmabuf uninstall

# Separate rule to be called from DKMS
install_aux: $(DTBO)
	# Install device tree overlay
	install -D -m 0644 $< $(OVERLAY_DIR)/sharp-drm.dtbo
	# Install udev rules for module parameters and backlight controls
	install -D -m 0644 $(UDEV_RULE) \
		$(UDEV_RULE_DIR)/$(notdir $(UDEV_RULE))
	# Add configuration line if it wasn't already there
	#@grep -qxF '$(BOOT_CONFIG_LINE)' $(CONFIG) \
	#	|| printf '[all]\ndtparam=spi=on\n$(BOOT_CONFIG_LINE)\n' >> $(CONFIG)
	# Add auto-load module line if it wasn't already there
	@grep -qxF 'sharp-drm' /etc/modules \
		|| echo 'sharp-drm' >> /etc/modules
	# Configure fbcon for display
	#@grep -qF '$(BOOT_CMDLINE_ADD)' $(CMDLINE) \
	#	|| sed -i.save 's/$$/ $(BOOT_CMDLINE_ADD)/' $(CMDLINE)

uninstall:
	# Remove fbcon configuration and create a backup file
	@sed -i.save 's/ $(BOOT_CMDLINE_ADD)//' $(CMDLINE)
	# Remove auto-load module line and create a backup file
	@sed -i.save '/sharp-drm/d' /etc/modules
	# Remove configuration line and create a backup file
	@sed -i.save '/$(BOOT_CONFIG_LINE)/d' $(CONFIG)
	# Remove device tree overlay
	@rm -f $(OVERLAY_DIR)/sharp-drm.dtbo
	# Remove udev rules
	@rm -f $(UDEV_RULE_DIR)/$(notdir $(UDEV_RULE))

clean_modules:
	$(MAKE) -C '$(LINUX_DIR)' M='$(shell pwd)' clean

clean: clean_modules
	rm -f overlays/*.dtbo $(REDRAW_BINARY)
	$(MAKE) -C contrib/qml-dmabuf clean
