#include "matoya.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

// XXX Required because clicks does not always fire on the edges
#define EDGE_PADDING 1.0f

#define VALIDATE_CTX(ctx, ...) if (!ctx) return __VA_ARGS__

struct MTY_Zoom {
	MTY_InputMode mode;
	bool scaling;
	bool relative;
	bool postpone;

	MTY_Point image;
	MTY_Point image_min;
	MTY_Point image_max;
	MTY_Point focus;

	MTY_Point origin;
	MTY_Point cursor;
	float margin;
	float status;

	float image_w;
	float image_h;
	float window_w;
	float window_h;

	float scale_screen;
	float scale_screen_min;
	float scale_screen_max;
	float scale_image;
	float scale_image_min;
	float scale_image_max;
};

MTY_Zoom *MTY_ZoomCreate() 
{
	MTY_Zoom *ctx = MTY_Alloc(1, sizeof(MTY_Zoom));

	ctx->mode = MTY_INPUT_MODE_TOUCHSCREEN;

	ctx->scale_screen = 1;
	ctx->scale_screen_min = 1;
	ctx->scale_screen_max = 4;

	return ctx;
}

void MTY_ZoomDestroy(MTY_Zoom **zoom)
{
	if (!zoom || !*zoom)
		return;

	MTY_Zoom *ctx = *zoom;

	MTY_Free(ctx);
	*zoom = NULL;
}

static bool mty_zoom_context_initialized(MTY_Zoom *ctx)
{
	return ctx && ctx->window_w && ctx->window_h && ctx->image_w && ctx->image_h;
}

static float mty_zoom_transform_x(MTY_Zoom *ctx, float value)
{
	float offset_x = -ctx->image.x / ctx->scale_screen + ctx->image_min.x;
	float zoom_w   = ctx->window_w / ctx->scale_screen;
	float ratio_x  = value / ctx->window_w;

	return offset_x + zoom_w * ratio_x;
}

static float mty_zoom_transform_y(MTY_Zoom *ctx, float value)
{
	float offset_y = -ctx->image.y / ctx->scale_screen + ctx->image_min.y;
	float zoom_h   = ctx->window_h / ctx->scale_screen;
	float ratio_y  = value / ctx->window_h;

	return offset_y + zoom_h * ratio_y;
}

static void mty_zoom_restrict_image(MTY_Zoom *ctx)
{
	float image_scaled_w = ctx->image_w * ctx->scale_image;
	float image_scaled_h = ctx->image_h * ctx->scale_image;

	if (ctx->image.x > ctx->image_min.x)
		ctx->image.x = ctx->image_min.x;

	if (ctx->image.y > ctx->image_min.y)
		ctx->image.y = ctx->image_min.y;

	if (ctx->image.x < ctx->image_max.x - image_scaled_w)
		ctx->image.x = ctx->image_max.x - image_scaled_w;

	if (ctx->image.y < ctx->image_max.y - image_scaled_h)
		ctx->image.y = ctx->image_max.y - image_scaled_h;
}

void MTY_ZoomUpdate(MTY_Zoom *ctx, uint32_t windowWidth, uint32_t windowHeight, uint32_t imageWidth, uint32_t imageHeight)
{
	VALIDATE_CTX(ctx);

	bool same_window = lrint(ctx->window_w) == (long) windowWidth && lrint(ctx->window_h) == (long) windowHeight;
	bool same_image  = lrint(ctx->image_w)  == (long) imageWidth  && lrint(ctx->image_h)  == (long) imageHeight;
	if (same_window && same_image)
		return;

	ctx->window_w = (float) windowWidth;
	ctx->window_h = (float) windowHeight;
	ctx->image_w = (float) imageWidth;
	ctx->image_h = (float) imageHeight;

	ctx->scale_screen = 1;
	float scale_w = ctx->window_w / ctx->image_w;
	float scale_h = ctx->window_h / ctx->image_h;
	ctx->scale_image = scale_w < scale_h ? scale_w : scale_h;

	ctx->image.x = 0;
	ctx->image.y = 0;
	if (scale_w > scale_h) 
		ctx->image.x = (ctx->window_w - ctx->image_w * ctx->scale_image) / 2.0f;
	if (scale_w < scale_h) 
		ctx->image.y = (ctx->window_h - ctx->image_h * ctx->scale_image) / 2.0f;

	ctx->image_min.x = ctx->image.x;
	ctx->image_min.y = ctx->image.y;
	ctx->image_max.x = ctx->window_w - ctx->image.x;
	ctx->image_max.y = ctx->window_h - ctx->image.y;

	ctx->scale_image_min = ctx->scale_image * ctx->scale_screen_min;
	ctx->scale_image_max = ctx->scale_image * ctx->scale_screen_max;

	ctx->cursor.x = ctx->window_w / 2.0f;
	ctx->cursor.y = ctx->window_h / 2.0f;

	ctx->margin = (ctx->window_w < ctx->window_h ? ctx->window_w : ctx->window_h) * 0.2f;

	ctx->focus.x = 0;
	ctx->focus.y = 0;
}

void MTY_ZoomScale(MTY_Zoom *ctx, float scaleFactor, float focusX, float focusY)
{
	VALIDATE_CTX(ctx);

	if (!mty_zoom_context_initialized(ctx))
		return;

	if (ctx->scaling) {
		ctx->image.x += focusX - ctx->focus.x;
		ctx->image.y += focusY - ctx->focus.y;
	}

	ctx->focus.x = focusX;
	ctx->focus.y = focusY;

	ctx->scale_screen *= scaleFactor;
	ctx->scale_image  *= scaleFactor;
	
	if (ctx->scale_screen < ctx->scale_screen_min) {
		ctx->scale_screen = ctx->scale_screen_min;
		ctx->scale_image  = ctx->scale_image_min;
		scaleFactor = 1;

	} else if (ctx->scale_screen > ctx->scale_screen_max) {
		ctx->scale_screen = ctx->scale_screen_max;
		ctx->scale_image  = ctx->scale_image_max;
		scaleFactor = 1;
	}

	ctx->image.x = ctx->focus.x - scaleFactor * (ctx->focus.x - ctx->image.x);
	ctx->image.y = ctx->focus.y - scaleFactor * (ctx->focus.y - ctx->image.y);

	mty_zoom_restrict_image(ctx);

	if (ctx->scaling) {
		ctx->cursor.x = mty_zoom_transform_x(ctx, ctx->window_w / 2.0f);
		ctx->cursor.y = mty_zoom_transform_y(ctx, ctx->window_h / 2.0f);
	}
}

void MTY_ZoomMove(MTY_Zoom *ctx, int32_t x, int32_t y, bool start)
{
	VALIDATE_CTX(ctx);

	if (!mty_zoom_context_initialized(ctx) || ctx->scaling)
		return;

	if (ctx->mode == MTY_INPUT_MODE_TOUCHSCREEN) {
		ctx->cursor.x = mty_zoom_transform_x(ctx, (float) x);
		ctx->cursor.y = mty_zoom_transform_y(ctx, (float) y);
		return;
	}

	if (start || ctx->postpone) {
		ctx->postpone = ctx->relative;

		if (ctx->postpone)
			return;

		ctx->origin.x = (float) x;
		ctx->origin.y = (float) y;
		return;
	}

	float delta_x = x - ctx->origin.x;
	float delta_y = y - ctx->origin.y;

	ctx->cursor.x += delta_x / ctx->scale_screen;
	ctx->cursor.y += delta_y / ctx->scale_screen;

	if (ctx->cursor.x < ctx->image_min.x + EDGE_PADDING)
		ctx->cursor.x = ctx->image_min.x + EDGE_PADDING;

	if (ctx->cursor.y < ctx->image_min.y + EDGE_PADDING)
		ctx->cursor.y = ctx->image_min.y + EDGE_PADDING;

	if (ctx->cursor.x > ctx->window_w - ctx->image_min.x - EDGE_PADDING)
		ctx->cursor.x = ctx->window_w - ctx->image_min.x - EDGE_PADDING;

	if (ctx->cursor.y > ctx->window_h - ctx->image_min.y - EDGE_PADDING)
		ctx->cursor.y = ctx->window_h - ctx->image_min.y - EDGE_PADDING;

	float left   = mty_zoom_transform_x(ctx, ctx->margin);
	float right  = mty_zoom_transform_x(ctx, ctx->window_w - ctx->margin);
	float top    = mty_zoom_transform_y(ctx, ctx->margin);
	float bottom = mty_zoom_transform_y(ctx, ctx->window_h - ctx->margin);

	if (delta_x < 0 && ctx->cursor.x < left)
		ctx->image.x -= delta_x;

	if (delta_x > 0 && ctx->cursor.x > right)
		ctx->image.x -= delta_x;

	if (delta_y < 0 && ctx->cursor.y < top)
		ctx->image.y -= delta_y;

	if (delta_y > 0 && ctx->cursor.y > bottom)
		ctx->image.y -= delta_y;

	mty_zoom_restrict_image(ctx);

	ctx->origin.x = (float) x;
	ctx->origin.y = (float) y;
}

int32_t MTY_ZoomTransformX(MTY_Zoom *ctx, int32_t value)
{
	VALIDATE_CTX(ctx, value);

	if (ctx->relative)
		return lrint(value / ctx->scale_screen);

	if (ctx->mode == MTY_INPUT_MODE_TRACKPAD)
		return lrint(ctx->cursor.x);

	return lrint(mty_zoom_transform_x(ctx, (float) value));
}

int32_t MTY_ZoomTransformY(MTY_Zoom *ctx, int32_t value)
{
	VALIDATE_CTX(ctx, value);

	if (ctx->relative)
		return lrint(value / ctx->scale_screen);

	if (ctx->mode == MTY_INPUT_MODE_TRACKPAD)
		return lrint(ctx->cursor.y);

	return lrint(mty_zoom_transform_y(ctx, (float) value));
}

float MTY_ZoomGetScale(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, 1);

	return ctx->scale_image;
}

int32_t MTY_ZoomGetImageX(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, 0);

	return lrint(ctx->image.x);
}

int32_t MTY_ZoomGetImageY(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, 0);

	return lrint(ctx->image.y);
}

int32_t MTY_ZoomGetCursorX(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, 0);

	float left  = mty_zoom_transform_x(ctx, 0);
	float right = mty_zoom_transform_x(ctx, ctx->window_w);

	return lrint(ctx->window_w * (ctx->cursor.x - left) / (right - left));
}

int32_t MTY_ZoomGetCursorY(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, 0);

	float top    = mty_zoom_transform_y(ctx, 0);
	float bottom = mty_zoom_transform_y(ctx, ctx->window_h);

	return lrint(ctx->window_h * (ctx->cursor.y - top) / (bottom - top));
}

bool MTY_ZoomIsScaling(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, false);

	return ctx->scaling;
}

void MTY_ZoomSetScaling(MTY_Zoom *ctx, bool scaling)
{
	VALIDATE_CTX(ctx);

	ctx->scaling = scaling;
}

bool MTY_ZoomIsRelative(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, false);

	return ctx->relative;
}

void MTY_ZoomSetRelative(MTY_Zoom *ctx, bool relative)
{
	VALIDATE_CTX(ctx);

	ctx->relative = relative;
}

bool MTY_ZoomIsTrackpadEnabled(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, false);

	return ctx->mode == MTY_INPUT_MODE_TRACKPAD;
}

void MTY_ZoomEnableTrackpad(MTY_Zoom *ctx, bool enable)
{
	VALIDATE_CTX(ctx);

	ctx->mode = enable ? MTY_INPUT_MODE_TRACKPAD : MTY_INPUT_MODE_TOUCHSCREEN;
}

bool MTY_ZoomHasMoved(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, false);

	float status = ctx->cursor.x + ctx->cursor.y + ctx->image.x + ctx->image.y;

	bool has_moved = status != ctx->status;
	ctx->status = status;

	return has_moved || ctx->scaling;
}

bool MTY_ZoomShouldShowCursor(MTY_Zoom *ctx)
{
	VALIDATE_CTX(ctx, false);

	return ctx->mode == MTY_INPUT_MODE_TRACKPAD && !ctx->relative;
}

void MTY_ZoomSetLimits(MTY_Zoom *ctx, float min, float max)
{
	VALIDATE_CTX(ctx);

	ctx->scale_screen_min = min;
	ctx->scale_screen_max = max;
}
