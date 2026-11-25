/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * struct s13c_bus over the BF70x ST7796S driver. ctx is the struct wb_lcd
 * (the first member of struct wb_display_bf70x).
 */
#include "waffle/drivers/bf70x/display.h"

/* cppcheck-suppress constParameterCallback ; s13c_bus fixes the type */
static void a_reset(void *ctx, bool high)
{
	wb_lcd_reset(ctx, high);
}

static void a_select(void *ctx, bool on)
{
	wb_lcd_select(ctx, on);
}

static void a_cmd(void *ctx, uint8_t c)
{
	wb_lcd_cmd(ctx, c);
}

static void a_data(void *ctx, const uint8_t *p, size_t n)
{
	wb_lcd_data(ctx, p, n);
}

static void a_repeat(void *ctx, const uint8_t *group, size_t len,
		     uint32_t count)
{
	wb_lcd_repeat(ctx, group, len, count);
}

static int a_read(void *ctx, uint8_t cmd, uint8_t *out, size_t n)
{
	return wb_lcd_read(ctx, cmd, out, n);
}

static int a_frame_start(void *ctx, const uint8_t *buf, uint32_t n)
{
	return wb_lcd_frame_start(ctx, buf, n);
}

static bool a_frame_busy(void *ctx)
{
	return wb_lcd_frame_busy(ctx);
}

static void a_frame_stop(void *ctx)
{
	wb_lcd_frame_stop(ctx);
}

/* cppcheck-suppress constParameterCallback ; s13c_bus fixes the type */
static int a_te_wait(void *ctx, uint32_t timeout_us)
{
	return wb_lcd_te_wait(ctx, timeout_us);
}

/* cppcheck-suppress constParameterCallback ; s13c_bus fixes the type */
static uint64_t a_now_us(void *ctx)
{
	const struct wb_lcd *l = ctx;

	return wb_time_us(l->time);
}

/* cppcheck-suppress constParameterCallback ; s13c_bus fixes the type */
static void a_delay_us(void *ctx, uint32_t us)
{
	const struct wb_lcd *l = ctx;

	wb_delay_us(l->time, us);
}

const struct s13c_bus *wb_display_bf70x_init(struct wb_display_bf70x *a,
					     const struct wb_time *t)
{
	return wb_display_bf70x_init_cfg(a, &wb_lcd_opxy, t);
}

const struct s13c_bus *wb_display_bf70x_init_cfg(struct wb_display_bf70x *a,
						 const struct wb_lcd_config *cfg,
						 const struct wb_time *t)
{
	struct s13c_bus *b = &a->bus;

	if (wb_lcd_init(&a->lcd, cfg, t) != WB_LCD_OK)
		return 0;
	b->reset = a_reset;
	b->select = a_select;
	b->cmd = a_cmd;
	b->data = a_data;
	b->repeat = a_repeat;
	b->read = a_read;
	b->frame_start = a_frame_start;
	b->frame_busy = a_frame_busy;
	b->frame_stop = a_frame_stop;
	b->te_wait = a_te_wait;
	b->now_us = a_now_us;
	b->delay_us = a_delay_us;
	b->ctx = &a->lcd;
	return b;
}
