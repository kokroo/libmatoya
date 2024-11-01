// This Source Code Form is subject to the terms of the MIT License.
// If a copy of the MIT License was not distributed with this file,
// You can obtain one at https://spdx.org/licenses/MIT.html.

#include "webview.h"

#include <string.h>
#include <math.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <webkit2/webkit2.h>

#include "matoya.h"
#include "web/keymap.h"

#define DISPATCH(func, param, should_free) g_idle_add(G_SOURCE_FUNC(func), mty_webview_create_event(ctx, (void *) (param), should_free))

struct webview {
	MTY_App *app;
	MTY_Window window;
	WEBVIEW_READY ready_func;
	WEBVIEW_TEXT text_func;
	WEBVIEW_KEY key_func;
	MTY_Hash *keys;
	MTY_Queue *pushq;
	bool ready;
	bool passthrough;
	bool debug;

	MTY_Thread *thread;
	Display *display;
	Window x11_window;
	GtkWindow *gtk_window;
	WebKitWebView *webview;
};

struct mty_webview_event {
	struct webview *context;
	void *data;
	bool should_free;
};

struct xinfo {
	Display *display;
	XVisualInfo *vis;
	Window window;
};

static struct mty_webview_event *mty_webview_create_event(struct webview *ctx, void *data, bool should_free)
{
	struct mty_webview_event *event = MTY_Alloc(1, sizeof(struct mty_webview_event));

	event->context = ctx;
	event->data = data;
	event->should_free = should_free;

	return event;
}

static void mty_webview_destroy_event(struct mty_webview_event **event)
{
	if (!event || !*event)
		return;

	struct mty_webview_event *ev = *event;

	if (ev->should_free)
		MTY_Free(ev->data);

	MTY_Free(ev);
	*event = NULL;
}

static void handle_script_message(WebKitUserContentManager *manager, WebKitJavascriptResult *result, void *opaque)
{
	struct webview *ctx = opaque;

	JSCValue *value = webkit_javascript_result_get_js_value(result);
	char *str = jsc_value_to_string(value);

	MTY_JSON *j = NULL;

	switch (str[0]) {
		// MTY_EVENT_WEBVIEW_READY
		case 'R':
			ctx->ready = true;

			// Send any queued messages before the WebView became ready
			for (char *msg = NULL; MTY_QueuePopPtr(ctx->pushq, 0, (void **) &msg, NULL);) {
				mty_webview_send_text(ctx, msg);
				MTY_Free(msg);
			}

			ctx->ready_func(ctx->app, ctx->window);
			break;

		// MTY_EVENT_WEBVIEW_TEXT
		case 'T':
			ctx->text_func(ctx->app, ctx->window, str + 1);
			break;

		// MTY_EVENT_KEY
		case 'D':
		case 'U':
			if (!ctx->passthrough)
				break;

			j = MTY_JSONParse(str + 1);
			if (!j)
				break;

			const char *code = MTY_JSONObjGetStringPtr(j, "code");
			if (!code)
				break;

			uint32_t jmods = 0;
			if (!MTY_JSONObjGetInt(j, "mods", (int32_t *) &jmods))
				break;

			MTY_Key key = (MTY_Key) (uintptr_t) MTY_HashGet(ctx->keys, code) & 0xFFFF;
			if (key == MTY_KEY_NONE)
				break;

			MTY_Mod mods = web_keymap_mods(jmods);

			ctx->key_func(ctx->app, ctx->window, str[0] == 'D', key, mods);
			break;
	}

	MTY_JSONDestroy(&j);
	MTY_Free(str);
}

static bool _mty_webview_resize(void *opaque)
{
	struct webview *ctx = opaque;

	XWindowAttributes attr = {0};
	XGetWindowAttributes(ctx->display, ctx->x11_window, &attr);

	int32_t width = 0, height = 0;
	gtk_window_get_size(ctx->gtk_window, &width, &height);

	if (width != attr.width || height != attr.height)
		gtk_window_resize(ctx->gtk_window, attr.width, attr.height);

	return true;
}

static bool _mty_webview_create(struct mty_webview_event *event)
{
	struct webview *ctx = event->context;

	ctx->gtk_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_POPUP));
	gtk_widget_realize(GTK_WIDGET(ctx->gtk_window));

	GdkWindow *gdk_window = gtk_widget_get_window(GTK_WIDGET(ctx->gtk_window));
	XReparentWindow(GDK_WINDOW_XDISPLAY(gdk_window), GDK_WINDOW_XID(gdk_window), ctx->x11_window, 0, 0);

	ctx->webview = WEBKIT_WEB_VIEW(webkit_web_view_new());
	gtk_container_add(GTK_CONTAINER(ctx->gtk_window), GTK_WIDGET(ctx->webview));

	gtk_widget_set_app_paintable(GTK_WIDGET(ctx->gtk_window), TRUE);
	webkit_web_view_set_background_color(ctx->webview, & (GdkRGBA) {0});

	WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager(ctx->webview);
	g_signal_connect(manager, "script-message-received::native", G_CALLBACK(handle_script_message), ctx);
	webkit_user_content_manager_register_script_message_handler(manager, "native");

	const char *javascript =
		"const __MTY_MSGS = [];"

		"window.addEventListener('message', evt => {"
			"if (window.MTY_NativeListener) {"
				"window.MTY_NativeListener(evt.data);"

			"} else {"
				"__MTY_MSGS.push(evt.data);"
			"}"
		"});"

		"window.MTY_NativeSendText = text => {"
			"window.webkit.messageHandlers.native.postMessage('T' + text);"
		"};"

		"window.webkit.messageHandlers.native.postMessage('R');"

		"const __MTY_INTERVAL = setInterval(() => {"
			"if (window.MTY_NativeListener) {"
				"for (let msg = __MTY_MSGS.shift(); msg; msg = __MTY_MSGS.shift())"
					"window.MTY_NativeListener(msg);"

				"clearInterval(__MTY_INTERVAL);"
			"}"
		"}, 100);"

		"function __mty_key_to_json(evt) {"
			"let mods = 0;"

			"if (evt.shiftKey) mods |= 0x01;"
			"if (evt.ctrlKey)  mods |= 0x02;"
			"if (evt.altKey)   mods |= 0x04;"
			"if (evt.metaKey)  mods |= 0x08;"

			"if (evt.getModifierState('CapsLock')) mods |= 0x10;"
			"if (evt.getModifierState('NumLock')) mods |= 0x20;"

			"let cmd = evt.type == 'keydown' ? 'D' : 'U';"
			"let json = JSON.stringify({'code':evt.code,'mods':mods});"

			"window.webkit.messageHandlers.native.postMessage(cmd + json);"
		"}"

		"document.addEventListener('keydown', __mty_key_to_json);"
		"document.addEventListener('keyup', __mty_key_to_json);";

	WebKitUserContentInjectedFrames injected_frames = WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
	WebKitUserScriptInjectionTime injection_time = WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START;
	WebKitUserScript *script = webkit_user_script_new(javascript, injected_frames, injection_time, NULL, NULL);
	webkit_user_content_manager_add_script(manager, script);

	WebKitSettings *settings = webkit_web_view_get_settings(event->context->webview);
	webkit_settings_set_enable_developer_extras(settings, ctx->debug);

	g_idle_add(_mty_webview_resize, ctx);

	gtk_widget_show_all(GTK_WIDGET(ctx->gtk_window));

	mty_webview_destroy_event(&event);

	return false;
}

static void *mty_webview_thread_func(void *opaque)
{
	gtk_init_check(0, NULL);
	gtk_main();

	return NULL;
}

struct webview *mty_webview_create(MTY_App *app, MTY_Window window, const char *dir,
	bool debug, WEBVIEW_READY ready_func, WEBVIEW_TEXT text_func, WEBVIEW_KEY key_func)
{
	struct webview *ctx = MTY_Alloc(1, sizeof(struct webview));

	g_setenv("GDK_BACKEND", "x11", true);

	ctx->app = app;
	ctx->window = window;
	ctx->ready_func = ready_func;
	ctx->text_func = text_func;
	ctx->key_func = key_func;
	ctx->debug = debug;

	ctx->keys = web_keymap_hash();
	ctx->pushq = MTY_QueueCreate(50, 0);
	ctx->thread = MTY_ThreadCreate(mty_webview_thread_func, NULL);

	struct xinfo *info = MTY_WindowGetNative(ctx->app, ctx->window);
	ctx->display = info->display;
	ctx->x11_window = info->window;

	DISPATCH(_mty_webview_create, NULL, false);

	return ctx;
}

static bool _mty_webview_destroy(struct mty_webview_event *event)
{
	struct webview *ctx = event->context;

	if (!ctx)
		return false;

	gtk_window_close(ctx->gtk_window);
	gtk_widget_destroy(GTK_WIDGET(ctx->webview));
	gtk_widget_destroy(GTK_WIDGET(ctx->gtk_window));
	gtk_main_quit();

	MTY_Free(ctx);

	mty_webview_destroy_event(&event);

	return false;
}

void mty_webview_destroy(struct webview **webview)
{
	if (!webview || !*webview)
		return;

	struct webview *ctx = *webview;
	*webview = NULL;

	DISPATCH(_mty_webview_destroy, NULL, false);

	MTY_ThreadDestroy(&ctx->thread);

	if (ctx->pushq)
		MTY_QueueFlush(ctx->pushq, MTY_Free);

	MTY_QueueDestroy(&ctx->pushq);
	MTY_HashDestroy(&ctx->keys, NULL);

	MTY_Free(ctx);
}

static bool _mty_webview_navigate_url(struct mty_webview_event *event)
{
	webkit_web_view_load_uri(event->context->webview, event->data);
	mty_webview_destroy_event(&event);

	return false;
}

static bool _mty_webview_navigate_html(struct mty_webview_event *event)
{
	webkit_web_view_load_html(event->context->webview, event->data, NULL);
	mty_webview_destroy_event(&event);

	return false;
}

static bool _mty_webview_show(struct mty_webview_event *event)
{
	GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(event->context->gtk_window));

	if (event->data) {
		gdk_window_show(window);

	} else {
		gdk_window_hide(window);
	}

	return false;
}

static bool _mty_webview_send_text(struct mty_webview_event *event)
{
	// Need to escape backslash !
	MTY_JSON *json = MTY_JSONStringCreate(event->data);
	char *text = MTY_JSONSerialize(json);
	char *message = MTY_SprintfD("window.postMessage(%s, '*');", text);
	webkit_web_view_run_javascript(event->context->webview, message, NULL, NULL, NULL);
	MTY_Free(message);
	MTY_Free(text);
	MTY_JSONDestroy(&json);

	return false;
}

static bool _mty_webview_reload(struct mty_webview_event *event)
{
	webkit_web_view_reload(event->context->webview);

	return false;
}

void mty_webview_navigate(struct webview *ctx, const char *source, bool url)
{
	if (url) {
		DISPATCH(_mty_webview_navigate_url, MTY_Strdup(source), true);

	} else {
		DISPATCH(_mty_webview_navigate_html, MTY_Strdup(source), true);
	}
}

void mty_webview_show(struct webview *ctx, bool show)
{
	DISPATCH(_mty_webview_show, show, false);
}

bool mty_webview_is_visible(struct webview *ctx)
{
	return gdk_window_is_visible(gtk_widget_get_window(GTK_WIDGET(ctx->gtk_window)));
}

void mty_webview_send_text(struct webview *ctx, const char *msg)
{
	DISPATCH(_mty_webview_send_text, MTY_Strdup(msg), true);
}

void mty_webview_reload(struct webview *ctx)
{
	DISPATCH(_mty_webview_reload, NULL, false);
}

void mty_webview_set_input_passthrough(struct webview *ctx, bool passthrough)
{
	ctx->passthrough = passthrough;
}

bool mty_webview_event(struct webview *ctx, MTY_Event *evt)
{
	return false;
}

void mty_webview_run(struct webview *ctx)
{
}

void mty_webview_render(struct webview *ctx)
{
}

bool mty_webview_is_focussed(struct webview *ctx)
{
	return true;
}

bool mty_webview_is_steam(void)
{
	return false;
}

bool mty_webview_is_available(void)
{
	return true;
}
