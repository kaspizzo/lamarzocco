ROOT_DIR := $(abspath $(CURDIR)/../..)
LVGL_DIR := $(ROOT_DIR)/firmware/esp32/managed_components/lvgl__lvgl
OBJDIR := $(CURDIR)/build
OUT_DIR := $(CURDIR)/out
DOCS_DIR := $(ROOT_DIR)/docs/controller/images
PORTAL_PREVIEW := $(CURDIR)/controller_setup_portal_preview.html
DEVICE_SHOTS := boiler prebrew-on-time prebrew-off-time steam-boiler status preset-view first-setup setup-with-wlan-connected

include $(LVGL_DIR)/src/core/lv_core.mk
include $(LVGL_DIR)/src/draw/lv_draw.mk
include $(LVGL_DIR)/src/extra/lv_extra.mk
include $(LVGL_DIR)/src/font/lv_font.mk
include $(LVGL_DIR)/src/hal/lv_hal.mk
include $(LVGL_DIR)/src/misc/lv_misc.mk
include $(LVGL_DIR)/src/widgets/lv_widgets.mk

CSRCS += $(ROOT_DIR)/firmware/esp32/main/controller_ui.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/controller_connectivity.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/fonts/lm_ctrl_font_montserrat_14.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/fonts/lm_ctrl_font_montserrat_16.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/fonts/lm_ctrl_font_montserrat_20.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/fonts/lm_ctrl_font_montserrat_28.c
CSRCS += $(ROOT_DIR)/firmware/esp32/main/fonts/lm_ctrl_font_montserrat_40.c
CSRCS += $(CURDIR)/renderer.c
CSRCS += $(CURDIR)/stubs.c

CFLAGS += -I$(CURDIR)
CFLAGS += -I$(CURDIR)/include
CFLAGS += -I$(LVGL_DIR)
CFLAGS += -I$(LVGL_DIR)/src
CFLAGS += -I$(ROOT_DIR)/firmware/esp32/main
CFLAGS += -DLV_CONF_INCLUDE_SIMPLE
CFLAGS += -Wall -Wextra -Wno-unused-parameter

define obj_for
$(OBJDIR)/$(subst /,_,$(basename $(1))).o
endef

COBJS := $(foreach src,$(CSRCS),$(call obj_for,$(src)))

define make_obj_rule
$(call obj_for,$(1)): $(1)
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $$< -o $$@
endef

$(foreach src,$(CSRCS),$(eval $(call make_obj_rule,$(src))))

renderer: $(COBJS)
	$(CC) -o $@ $^ -lm

device-shots: renderer
	@mkdir -p $(OUT_DIR)
	./renderer

portal-shot: $(PORTAL_PREVIEW)
	@mkdir -p $(OUT_DIR)
	qlmanage -t -s 2100 -o $(OUT_DIR) $(PORTAL_PREVIEW) >/dev/null

export: device-shots portal-shot
	@mkdir -p $(DOCS_DIR)
	@for shot in $(DEVICE_SHOTS); do \
		sips -s format png $(OUT_DIR)/$$shot.bmp --out $(DOCS_DIR)/$$shot.png >/dev/null; \
	done
	cp $(OUT_DIR)/controller_setup_portal_preview.html.png $(DOCS_DIR)/controller-setup-portal.png

clean:
	rm -rf $(OBJDIR) renderer
	rm -f $(OUT_DIR)/*.bmp $(OUT_DIR)/controller_setup_portal_preview.html.png
