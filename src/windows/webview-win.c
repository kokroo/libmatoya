// This Source Code Form is subject to the terms of the MIT License.
// If a copy of the MIT License was not distributed with this file,
// You can obtain one at https://spdx.org/licenses/MIT.html.

#include "webview.h"

#include <stdio.h>

#include <windows.h>
#include <ole2.h>
#include <shlwapi.h>

#define COBJMACROS
#include "webview2.h"

// https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution#detect-if-a-webview2-runtime-is-already-installed
#define WEBVIEW_REG_PATH L"Software\\Microsoft\\EdgeUpdate\\%s\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

#if defined(_WIN64)
	#define WEBVIEW_DLL_PATH L"EBWebView\\x64\\EmbeddedBrowserWebView.dll"

#else
	#define WEBVIEW_DLL_PATH L"EBWebView\\x86\\EmbeddedBrowserWebView.dll"
#endif

typedef HRESULT (WINAPI *WEBVIEW_CREATE_FUNC)(uintptr_t _unknown0, uintptr_t _unknown1,
	const WCHAR *wdir, ICoreWebView2EnvironmentOptions *opts,
	ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

struct webview_handler0 {
	ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler handler;
	void *opaque;
};

struct webview_handler1 {
	ICoreWebView2CreateCoreWebView2ControllerCompletedHandler handler;
	void *opaque;
};

struct webview_handler2 {
	ICoreWebView2WebMessageReceivedEventHandler handler;
	void *opaque;
};

struct webview_handler3 {
	ICoreWebView2FocusChangedEventHandler handler;
	void *opaque;
};

struct webview {
	struct webview_base base;

	HMODULE lib;
	ICoreWebView2Controller2 *controller;
	ICoreWebView2 *webview;
	struct webview_handler0 handler0;
	struct webview_handler1 handler1;
	struct webview_handler2 handler2;
	struct webview_handler3 handler3; // GotFocus
	struct webview_handler3 handler4; // LostFocus
	ICoreWebView2EnvironmentOptions opts;
	WCHAR *source;
	bool url;
};


// Generic COM shims

static bool com_check_riid(REFIID in, REFIID check)
{
	return (!memcmp(in, &IID_IUnknown, sizeof(IID)) || !memcmp(in, check, sizeof(IID)));
}

static ULONG STDMETHODCALLTYPE com_AddRef(void *This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE com_Release(void *This)
{
	return 0;
}


// ICoreWebView2FocusChangedEventHandler

static HRESULT STDMETHODCALLTYPE h3_QueryInterface(void *This,
	REFIID riid, _COM_Outptr_ void **ppvObject)
{
	if (com_check_riid(riid, &IID_ICoreWebView2FocusChangedEventHandler)) {
		*ppvObject = This;
		return S_OK;
	}

	return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE h3_Invoke_GotFocus(ICoreWebView2FocusChangedEventHandler *This,
	ICoreWebView2Controller *sender, IUnknown *args)
{
	struct webview_handler3 *handler = (struct webview_handler3 *) This;
	struct webview *ctx = handler->opaque;

	ctx->base.focussed = true;
	if (mty_webview_is_visible(ctx))
		PostMessage(MTY_WindowGetNative(ctx->base.app, ctx->base.window), WM_SETFOCUS, 0, 0);

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE h3_Invoke_LostFocus(ICoreWebView2FocusChangedEventHandler *This,
	ICoreWebView2Controller *sender, IUnknown *args)
{
	struct webview_handler3 *handler = (struct webview_handler3 *) This;
	struct webview *ctx = handler->opaque;

	ctx->base.focussed = false;
	if (mty_webview_is_visible(ctx))
		PostMessage(MTY_WindowGetNative(ctx->base.app, ctx->base.window), WM_KILLFOCUS, 0, 0);

	return S_OK;
}


// ICoreWebView2WebMessageReceivedEventHandler

static HRESULT STDMETHODCALLTYPE h2_QueryInterface(void *This,
	REFIID riid, _COM_Outptr_ void **ppvObject)
{
	if (com_check_riid(riid, &IID_ICoreWebView2WebMessageReceivedEventHandler)) {
		*ppvObject = This;
		return S_OK;
	}

	return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE h2_Invoke(ICoreWebView2WebMessageReceivedEventHandler *This,
	ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args)
{
	struct webview_handler2 *handler = (struct webview_handler2 *) This;
	struct webview *ctx = handler->opaque;

	WCHAR *wstr = NULL;
	HRESULT e = ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &wstr);

	if (e == S_OK) {
		char *str = MTY_WideToMultiD(wstr);
		mty_webview_base_handle_event(&ctx->base, str);
		MTY_Free(str);
	}

	return e;
}


// ICoreWebView2CreateCoreWebView2ControllerCompletedHandler

static HRESULT STDMETHODCALLTYPE h1_query_interface(void *This,
	REFIID riid, _COM_Outptr_ void **ppvObject)
{
	if (com_check_riid(riid, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
		*ppvObject = This;
		return S_OK;
	}

	return E_NOINTERFACE;
}

static void webview_update_size(struct webview *ctx)
{
	MTY_Size size = MTY_WindowGetSize(ctx->base.app, ctx->base.window);

	RECT bounds = {0};
	bounds.right = size.w;
	bounds.bottom = size.h;

	ICoreWebView2Controller2_put_Bounds(ctx->controller, bounds);
}

static void webview_navigate(struct webview *ctx, WCHAR *source, bool url)
{
	if (url) {
		ICoreWebView2_Navigate(ctx->webview, source);

	} else {
		ICoreWebView2_NavigateToString(ctx->webview, source);
	}
}

static HRESULT STDMETHODCALLTYPE h1_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
	HRESULT errorCode, ICoreWebView2Controller *controller)
{
	struct webview_handler1 *handler = (struct webview_handler1 *) This;
	struct webview *ctx = handler->opaque;

	HRESULT e = ICoreWebView2Controller2_QueryInterface(controller, &IID_ICoreWebView2Controller2, &ctx->controller);
	if (e != S_OK)
		return e;

	mty_webview_show(ctx, false);

	ICoreWebView2Controller2_get_CoreWebView2(ctx->controller, &ctx->webview);

	COREWEBVIEW2_COLOR bg = {0};
	ICoreWebView2Controller2_put_DefaultBackgroundColor(ctx->controller, bg);

	webview_update_size(ctx);

	ICoreWebView2Settings *settings = NULL;
	ICoreWebView2_get_Settings(ctx->webview, &settings);
	ICoreWebView2Settings_put_AreDevToolsEnabled(settings, ctx->base.debug);
	ICoreWebView2Settings_put_AreDefaultContextMenusEnabled(settings, ctx->base.debug);
	ICoreWebView2Settings_put_IsZoomControlEnabled(settings, FALSE);
	ICoreWebView2Settings_Release(settings);

	ICoreWebView2Controller2_add_GotFocus(ctx->controller,
		(ICoreWebView2FocusChangedEventHandler *) &ctx->handler3, NULL);
	ICoreWebView2Controller2_add_LostFocus(ctx->controller,
		(ICoreWebView2FocusChangedEventHandler *) &ctx->handler4, NULL);

	EventRegistrationToken token = {0};
	ICoreWebView2_add_WebMessageReceived(ctx->webview,
		(ICoreWebView2WebMessageReceivedEventHandler *) &ctx->handler2, &token);

	const WCHAR *script = L"window.native = {"
		L"postMessage: (message) => window.chrome.webview.postMessage(message),"
		L"addEventListener: (listener) => window.chrome.webview.addEventListener('message', listener),"
	L"};";
	ICoreWebView2_AddScriptToExecuteOnDocumentCreated(ctx->webview, script, NULL);

	if (ctx->source)
		webview_navigate(ctx, ctx->source, ctx->url);

	return S_OK;
}


// ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler

static HRESULT STDMETHODCALLTYPE h0_QueryInterface(void *This,
	REFIID riid, _COM_Outptr_ void **ppvObject)
{
	if (com_check_riid(riid, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
		*ppvObject = This;
		return S_OK;
	}

	return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE h0_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
	HRESULT errorCode, ICoreWebView2Environment *env)
{
	struct webview_handler0 *handler = (struct webview_handler0 *) This;
	struct webview *ctx = handler->opaque;

	HWND hwnd = MTY_WindowGetNative(ctx->base.app, ctx->base.window);

	return ICoreWebView2Environment_CreateCoreWebView2Controller(env, hwnd,
		(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *) &ctx->handler1);
}


// ICoreWebView2EnvironmentOptions

static HRESULT STDMETHODCALLTYPE opts_QueryInterface(void *This,
	REFIID riid, _COM_Outptr_ void **ppvObject)
{
	if (com_check_riid(riid, &IID_ICoreWebView2EnvironmentOptions)) {
		*ppvObject = This;
		return S_OK;
	}

	return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE opts_get_AdditionalBrowserArguments(
	ICoreWebView2EnvironmentOptions *This, LPWSTR *value)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_put_AdditionalBrowserArguments(
	ICoreWebView2EnvironmentOptions *This, LPCWSTR value)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_get_Language(
	ICoreWebView2EnvironmentOptions *This, LPWSTR *value)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_put_Language(
	ICoreWebView2EnvironmentOptions *This, LPCWSTR value)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_get_TargetCompatibleBrowserVersion(
	ICoreWebView2EnvironmentOptions *This, LPWSTR *value)
{
	const WCHAR *src = L"89.0.774.44";
	size_t size = (wcslen(src) + 1) * sizeof(WCHAR);
	WCHAR *dst = CoTaskMemAlloc(size);
	memcpy(dst, src, size);

	*value = dst;

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE opts_put_TargetCompatibleBrowserVersion(
	ICoreWebView2EnvironmentOptions *This, LPCWSTR value)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_get_AllowSingleSignOnUsingOSPrimaryAccount(
	ICoreWebView2EnvironmentOptions *This, BOOL *allow)
{
	return E_FAIL;
}

static HRESULT STDMETHODCALLTYPE opts_put_AllowSingleSignOnUsingOSPrimaryAccount(
	ICoreWebView2EnvironmentOptions *This, BOOL allow)
{
	return E_FAIL;
}


// Vtables

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl VTBL0 = {
	.QueryInterface = h0_QueryInterface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.Invoke = h0_Invoke,
};

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl VTBL1 = {
	.QueryInterface = h1_query_interface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.Invoke = h1_Invoke,
};

static ICoreWebView2WebMessageReceivedEventHandlerVtbl VTBL2 = {
	.QueryInterface = h2_QueryInterface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.Invoke = h2_Invoke,
};

static ICoreWebView2FocusChangedEventHandlerVtbl VTBL3 = {
	.QueryInterface = h3_QueryInterface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.Invoke = h3_Invoke_GotFocus,
};

static ICoreWebView2FocusChangedEventHandlerVtbl VTBL4 = {
	.QueryInterface = h3_QueryInterface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.Invoke = h3_Invoke_LostFocus,
};

static ICoreWebView2EnvironmentOptionsVtbl VTBL5 = {
	.QueryInterface = opts_QueryInterface,
	.AddRef = com_AddRef,
	.Release = com_Release,
	.get_AdditionalBrowserArguments = opts_get_AdditionalBrowserArguments,
	.put_AdditionalBrowserArguments = opts_put_AdditionalBrowserArguments,
	.get_Language = opts_get_Language,
	.put_Language = opts_put_Language,
	.get_TargetCompatibleBrowserVersion = opts_get_TargetCompatibleBrowserVersion,
	.put_TargetCompatibleBrowserVersion = opts_put_TargetCompatibleBrowserVersion,
	.get_AllowSingleSignOnUsingOSPrimaryAccount = opts_get_AllowSingleSignOnUsingOSPrimaryAccount,
	.put_AllowSingleSignOnUsingOSPrimaryAccount = opts_put_AllowSingleSignOnUsingOSPrimaryAccount,
};


// Public

static bool webview_dll_path_clientstate(wchar_t *pathw, bool as_user)
{
	bool ok = false;
	DWORD flags = KEY_READ;

	HKEY hkey = as_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	if (!as_user)
		flags |= KEY_WOW64_32KEY;

	wchar_t reg_path[MAX_PATH] = {0};
	_snwprintf_s(reg_path, MAX_PATH, _TRUNCATE, WEBVIEW_REG_PATH, L"ClientState");

	HKEY key = NULL;
	LSTATUS r = RegOpenKeyEx(hkey, reg_path, 0, flags, &key);
	if (r != ERROR_SUCCESS)
		goto except;

	DWORD size = MAX_PATH * sizeof(wchar_t);
	wchar_t dll[MAX_PATH] = {0};
	r = RegQueryValueEx(key, L"EBWebView", 0, NULL, (BYTE *) dll, &size);
	if (r != ERROR_SUCCESS)
		goto except;

	_snwprintf_s(pathw, MTY_PATH_MAX, _TRUNCATE, L"%s\\%s", dll, WEBVIEW_DLL_PATH);

	ok = PathFileExists(pathw);

	except:

	if (key)
		RegCloseKey(key);

	return ok;
}

static bool webview_dll_path_client(wchar_t *pathw, bool as_user)
{
	bool ok = false;
	DWORD flags = KEY_READ;

	HKEY hkey = as_user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
	if (!as_user)
		flags |= KEY_WOW64_32KEY;

	wchar_t reg_path[MAX_PATH] = {0};
	_snwprintf_s(reg_path, MAX_PATH, _TRUNCATE, WEBVIEW_REG_PATH, L"Clients");

	HKEY key = NULL;
	LSTATUS r = RegOpenKeyEx(hkey, reg_path, 0, flags, &key);
	if (r != ERROR_SUCCESS)
		goto except;

	DWORD size = MAX_PATH * sizeof(wchar_t);
	wchar_t dll[MAX_PATH] = {0};
	r = RegQueryValueEx(key, L"location", 0, NULL, (BYTE *) dll, &size);
	if (r != ERROR_SUCCESS)
		goto except;

	size = MAX_PATH * sizeof(wchar_t);
	wchar_t version[MAX_PATH] = {0};
	r = RegQueryValueEx(key, L"pv", 0, NULL, (BYTE *) version, &size);
	if (r != ERROR_SUCCESS)
		goto except;

	_snwprintf_s(pathw, MTY_PATH_MAX, _TRUNCATE, L"%s\\%s\\%s", dll, version, WEBVIEW_DLL_PATH);

	ok = PathFileExists(pathw);

	except:

	if (key)
		RegCloseKey(key);

	return ok;
}

static bool webview_dll_path(wchar_t *pathw, bool as_user)
{
	// Try convenient ClientState first
	if (webview_dll_path_clientstate(pathw, as_user))
		return true;

	// Construct the base path manually if ClientState is not available
	return webview_dll_path_client(pathw, as_user);
}

static HMODULE webview_load_dll(void)
{
	wchar_t path[MTY_PATH_MAX] = {0};
	HMODULE ret = NULL;

	// Try system WebView
	if (webview_dll_path(path, false))
		ret = LoadLibrary(path);

	// Try user WebView
	if (!ret && webview_dll_path(path, true))
		ret = LoadLibrary(path);

	return ret;
}

struct webview *mty_webview_create(MTY_App *app, MTY_Window window, const char *dir,
	bool debug, WEBVIEW_READY ready_func, WEBVIEW_TEXT text_func, WEBVIEW_KEY key_func)
{
	struct webview *ctx = MTY_Alloc(1, sizeof(struct webview));

	mty_webview_base_create(&ctx->base, app, window, dir, debug, ready_func, text_func, key_func);

	ctx->handler0.handler.lpVtbl = &VTBL0;
	ctx->handler0.opaque = ctx;
	ctx->handler1.handler.lpVtbl = &VTBL1;
	ctx->handler1.opaque = ctx;
	ctx->handler2.handler.lpVtbl = &VTBL2;
	ctx->handler2.opaque = ctx;
	ctx->handler3.handler.lpVtbl = &VTBL3;
	ctx->handler3.opaque = ctx;
	ctx->handler4.handler.lpVtbl = &VTBL4;
	ctx->handler4.opaque = ctx;
	ctx->opts.lpVtbl = &VTBL5;

	WCHAR dirw[MTY_PATH_MAX] = {0};
	MTY_MultiToWide(dir ? dir : "webview-data", dirw, MTY_PATH_MAX);

	HRESULT e = E_FAIL;
	ctx->lib = webview_load_dll();
	if (!ctx->lib)
		goto except;

	WEBVIEW_CREATE_FUNC func = (WEBVIEW_CREATE_FUNC) GetProcAddress(ctx->lib,
		"CreateWebViewEnvironmentWithOptionsInternal");
	if (!func)
		goto except;

	e = func(1, 0, dirw, &ctx->opts, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *) &ctx->handler0);

	except:

	if (e != S_OK)
		mty_webview_destroy(&ctx);

	return ctx;
}

void mty_webview_destroy(struct webview **webview)
{
	if (!webview || !*webview)
		return;

	struct webview *ctx = *webview;

	if (ctx->controller)
		ICoreWebView2Controller2_Release(ctx->controller);

	if (ctx->lib)
		FreeLibrary(ctx->lib);

	mty_webview_base_destroy(&ctx->base);

	MTY_Free(ctx->source);

	MTY_Free(ctx);
	*webview = NULL;
}

void mty_webview_navigate(struct webview *ctx, const char *source, bool url)
{
	WCHAR *wsource = MTY_MultiToWideD(source);

	if (ctx->webview) {
		webview_navigate(ctx, wsource, url);
		MTY_Free(wsource);

	} else {
		MTY_Free(ctx->source);
		ctx->source = wsource;
		ctx->url = url;
	}
}

void mty_webview_show(struct webview *ctx, bool show)
{
	if (!ctx->controller)
		return;

	ICoreWebView2Controller2_put_IsVisible(ctx->controller, show);

	if (show)
		ICoreWebView2Controller2_MoveFocus(ctx->controller, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

bool mty_webview_is_visible(struct webview *ctx)
{
	if (!ctx || !ctx->controller)
		return false;

	BOOL visible = FALSE;
	ICoreWebView2Controller2_get_IsVisible(ctx->controller, &visible);

	return visible;
}

void mty_webview_send_text(struct webview *ctx, const char *msg)
{
	if (!ctx->base.ready) {
		MTY_QueuePushPtr(ctx->base.pushq, MTY_Strdup(msg), 0);

	} else {
		WCHAR *wmsg = MTY_MultiToWideD(msg);
		ICoreWebView2_PostWebMessageAsString(ctx->webview, wmsg);
		MTY_Free(wmsg);
	}
}

void mty_webview_reload(struct webview *ctx)
{
	if (!ctx->webview)
		return;

	ICoreWebView2_Reload(ctx->webview);
}

void mty_webview_set_input_passthrough(struct webview *ctx, bool passthrough)
{
	ctx->base.passthrough = passthrough;
}

bool mty_webview_event(struct webview *ctx, MTY_Event *evt)
{
	if (evt->type == MTY_EVENT_SIZE)
		webview_update_size(ctx);

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
	return ctx && ctx->base.focussed;
}

bool mty_webview_is_steam(void)
{
	return false;
}

bool mty_webview_is_available(void)
{
	wchar_t path[MTY_PATH_MAX] = {0};

	bool have_path = webview_dll_path(path, false);
	if (!have_path)
		have_path = webview_dll_path(path, true);

	// Loading the lib would be ideal to be sure, but repeated loads eventually cause issues from Windows not un-reserving memory.
	// https://forums.codeguru.com/showthread.php?60548-Is-there-a-limit-on-how-many-times-one-can-load-(and-free)-the-same-DLL-in-a-process&p=156821#post156821
	return have_path;
}