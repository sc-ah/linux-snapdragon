// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2022, Xilin Wu <strongtz@yeah.net>

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct r66455_display {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	bool prepared;
};

static inline struct r66455_display *to_r66455_display(struct drm_panel *panel)
{
	return container_of(panel, struct r66455_display, panel);
}

#define dsi_generic_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_generic_write(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

#define dsi_dcs_write_seq(dsi, seq...) do {				\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write_buffer(dsi, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static void r66455_display_reset(struct r66455_display *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int r66455_display_on(struct r66455_display *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	dsi_generic_write_seq(dsi, 0xf4,
			      0xff, 0xff, 0xff, 0x04, 0x00, 0xc0, 0xf0, 0x01,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xf0, 0x39,
			      0xc1, 0x01, 0x02);
	usleep_range(1000, 2000);
	dsi_generic_write_seq(dsi, 0xf4,
			      0xff, 0xff, 0xff, 0x68, 0x00, 0x00, 0xf0, 0x02,
			      0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0xf0, 0x00,
			      0x00, 0x00, 0x00);
	usleep_range(10000, 11000);
	dsi_dcs_write_seq(dsi, 0x53, 0x2c);
	dsi_dcs_write_seq(dsi, 0x51, 0x03, 0xff);
	dsi_dcs_write_seq(dsi, 0xb0, 0x80);
	dsi_dcs_write_seq(dsi, 0xe6, 0x01);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(150);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}
	msleep(150);

	return 0;
}

static int r66455_display_off(struct r66455_display *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	return 0;
}

static int r66455_display_prepare(struct drm_panel *panel)
{
	struct r66455_display *ctx = to_r66455_display(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (ctx->prepared)
		return 0;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	r66455_display_reset(ctx);

	ret = r66455_display_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	ctx->prepared = true;
	return 0;
}

static int r66455_display_unprepare(struct drm_panel *panel)
{
	struct r66455_display *ctx = to_r66455_display(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = r66455_display_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	ctx->prepared = false;
	return 0;
}

static const struct drm_display_mode r66455_display_mode = {
	.clock = (1080 + 26 + 2 + 36) * (2160 + 8 + 4 + 4) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 26,
	.hsync_end = 1080 + 26 + 2,
	.htotal = 1080 + 26 + 2 + 36,
	.vdisplay = 2160,
	.vsync_start = 2160 + 8,
	.vsync_end = 2160 + 8 + 4,
	.vtotal = 2160 + 8 + 4 + 4,
	.width_mm = 74,
	.height_mm = 131,
};

static int r66455_display_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &r66455_display_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs r66455_display_panel_funcs = {
	.prepare = r66455_display_prepare,
	.unprepare = r66455_display_unprepare,
	.get_modes = r66455_display_get_modes,
};

static int r66455_display_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
// correct values. If not, remove this function.
static int r66455_display_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops r66455_display_bl_ops = {
	.update_status = r66455_display_bl_update_status,
	.get_brightness = r66455_display_bl_get_brightness,
};

static struct backlight_device *
r66455_display_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 4095,
		.max_brightness = 4095,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &r66455_display_bl_ops, &props);
}

static int r66455_display_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct r66455_display *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vci";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &r66455_display_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ctx->panel.backlight = r66455_display_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static int r66455_display_remove(struct mipi_dsi_device *dsi)
{
	struct r66455_display *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id r66455_display_of_match[] = {
	{ .compatible = "syna,fhd-r66455" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, r66455_display_of_match);

static struct mipi_dsi_driver r66455_display_driver = {
	.probe = r66455_display_probe,
	.remove = r66455_display_remove,
	.driver = {
		.name = "panel-r66455-display",
		.of_match_table = r66455_display_of_match,
	},
};
module_mipi_dsi_driver(r66455_display_driver);

MODULE_AUTHOR("Xilin Wu <strongtz@yeah.net>");
MODULE_DESCRIPTION("DRM driver for ASUS TM r66455 cmd fhd amoled panel");
MODULE_LICENSE("GPL v2");
