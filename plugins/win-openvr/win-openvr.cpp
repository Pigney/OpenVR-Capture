//
// OpenVR Capture
// Forked by pigney
// Originally "OpenVR Capture input plugin for OBS" by Keijo "Kegetys" Ruotsalainen
//

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <d3d11.h>
#include <thread>
#include <atomic>
#include <future>
#include <stdint.h>

std::atomic<bool> init_inprog(false);

#include <algorithm>
#include <vector>

#pragma comment(lib, "d3d11.lib")

#include "headers/openvr.h"
#ifdef _WIN64
#pragma comment(lib, "lib/win64/openvr_api.lib")
#else
#pragma comment(lib, "lib/win32/openvr_api.lib")
#endif

#define blog(log_level, message, ...) \
	blog(log_level, "[win_openvr] " message, ##__VA_ARGS__)

#define debug(message, ...)                                                    \
	blog(LOG_DEBUG, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define info(message, ...)                                                    \
	blog(LOG_INFO, "[%s] " message, obs_source_get_name(context->source), \
	     ##__VA_ARGS__)
#define warn(message, ...)                 \
	blog(LOG_WARNING, "[%s] " message, \
	     obs_source_get_name(context->source), ##__VA_ARGS__)

struct win_openvr {
	obs_source_t *source;

	bool righteye;
	double active_aspect_ratio;
	bool ar_crop;

	gs_texture_t *texture;
	ID3D11Device *dev11;
	ID3D11DeviceContext *ctx11;
	ID3D11Resource *tex;
	ID3D11ShaderResourceView *mirrorSrv;

	ID3D11Texture2D *texCrop;

	ULONGLONG lastCheckTick;

	// Set in win_openvr_init, 0 until then.
	unsigned int device_width;
	unsigned int device_height;

	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;

	double scale_factor;
	int x_offset;
	int y_offset;

	bool initialized;
	bool active;
};

bool IsVRSystemInitialized = false;

static void vr_init(void *data, bool forced)
{
	struct win_openvr *context = (win_openvr *)data;

	if (context->initialized)
		return;

	init_inprog = true;

	// Dont attempt to init OpenVR too often to reduce CPU usage
	if (GetTickCount64() - 1000 < context->lastCheckTick && !forced) {
		init_inprog = false;
		return;
	}

	// Init OpenVR, create D3D11 device and get shared mirror texture
	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Background);
	if (err != vr::VRInitError_None) {
		debug("OpenVR not available");
		// OpenVR not available
		context->lastCheckTick = GetTickCount64();
		init_inprog = false;
		return;
	}
	IsVRSystemInitialized = true;

	HRESULT hr;
	D3D_FEATURE_LEVEL featureLevel;
	hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0,
			       D3D11_SDK_VERSION, &context->dev11,
			       &featureLevel, &context->ctx11);
	if (FAILED(hr)) {
		warn("win_openvr_show: D3D11CreateDevice failed");
		init_inprog = false;
		return;
	}

	if (!vr::VRCompositor()) {
		warn("win_openvr_show: VR Compositor not found");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	vr::EVRCompositorError composError = vr::VRCompositor()->GetMirrorTextureD3D11(
		context->righteye ? vr::Eye_Right : vr::Eye_Left,
		context->dev11, (void **)&context->mirrorSrv);
	
	// Check for any compositor errors reported by OpenVR
	if (composError != vr::VRCompositorError_None || !context->mirrorSrv) {
		warn("win_openvr_show: GetMirrorTextureD3D11 failed, %d", composError);
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	// Get ID3D11Resource from shader resource view
	context->mirrorSrv->GetResource(&context->tex);
	if (!context->tex) {
		warn("win_openvr_show: GetResource failed");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	// Get the size from Texture2D
	ID3D11Texture2D *tex2D;
	context->tex->QueryInterface<ID3D11Texture2D>(&tex2D);
	if (!tex2D) {
		warn("win_openvr_show: QueryInterface failed");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	tex2D->GetDesc(&desc);
	if (desc.Width == 0 || desc.Height == 0) {
		warn("win_openvr_show: device width or height is 0");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}
	// Obtain display size
	context->device_width = desc.Width;
	context->device_height = desc.Height;

	// Pan and zoom
	int x = 0;
	int y = 0;

	double scale_factor = context->scale_factor;
	if (scale_factor < 1.0)
		scale_factor = 1.0;

	unsigned int scaled_width = static_cast<unsigned int>(context->device_width / scale_factor);
	unsigned int scaled_height = static_cast<unsigned int>(context->device_height / scale_factor);

	if (!context->righteye) {
		x = context->device_width - scaled_width;
	}

	context->width = scaled_width;
	context->height = scaled_height;

	// check for non-native AR, then proceed.
	if (context->ar_crop) {
		/// NEW CROP METHOD
		double input_aspect_ratio = static_cast<double>(context->width) / context->height;
		double active_aspect_ratio = context->active_aspect_ratio;

		if (input_aspect_ratio > active_aspect_ratio) {
			unsigned int cropped_width = static_cast<unsigned int>(context->height * active_aspect_ratio);
			context->width = cropped_width;
		} else if (input_aspect_ratio < active_aspect_ratio) {
			unsigned int cropped_height = static_cast<unsigned int>(context->width / active_aspect_ratio);
			context->height = cropped_height;
		}
		// END NEW CROP METHOD
	}

	int x_offset = context->x_offset;
	int y_offset = context->y_offset;

	if (!context->righteye) {
		x_offset = -x_offset;
	}

	x += x_offset;
	y += y_offset;

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + context->width > context->device_width) x = context->device_width - context->width;
	if (y + context->height > context->device_height) y = context->device_height - context->height;

	context->x = x;
	context->y = y;

	desc.Width = context->width;
	desc.Height = context->height;

	tex2D->Release();

	// Create cropped, linear texture
	// Using linear here will cause correct sRGB gamma to be applied
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
	hr = context->dev11->CreateTexture2D(&desc, NULL, &context->texCrop);
	if (FAILED(hr)) {
		warn("win_openvr_show: CreateTexture2D failed");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	// Get IDXGIResource, then share handle, and open it in OBS device
	IDXGIResource *res;
	hr = context->texCrop->QueryInterface(__uuidof(IDXGIResource),
					      (void **)&res);
	if (FAILED(hr)) {
		warn("win_openvr_show: QueryInterface failed");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	HANDLE handle = NULL;
	hr = res->GetSharedHandle(&handle);
	if (FAILED(hr)) {
		warn("win_openvr_show: GetSharedHandle failed");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}
	res->Release();

	#ifdef _WIN64
		uint32_t GShandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
	#else
		uint32_t GShandle = static_cast<uint32_t>(handle);
	#endif

	obs_enter_graphics();
	gs_texture_destroy(context->texture);
	context->texture = gs_texture_open_shared(GShandle);
	obs_leave_graphics();

	context->initialized = true;
	init_inprog = false;
}

std::future<void> init_future;

static void win_openvr_init(void *data, bool forced = true)
{
	struct win_openvr *context = (win_openvr *)data;

	if (context->initialized || init_inprog)
		return;
	if (!vr::VR_IsRuntimeInstalled()) {
		warn("win_openvr_show: SteamVR Runtime inactive!");
		return;
	}

	init_inprog = true;

	init_future = std::async(std::launch::async, vr_init, context, forced);
}

static void win_openvr_deinit(void *data)
{
	struct win_openvr *context = (win_openvr *)data;

	context->initialized = false;

	if (context->texture) {
		obs_enter_graphics();
		gs_texture_destroy(context->texture);
		obs_leave_graphics();
		context->texture = NULL;
	}

	if (context->tex)
		context->tex->Release();
	if (context->texCrop)
		context->texCrop->Release();
	//  if (context->mirrorSrv)
	//vr::VRCompositor()->ReleaseMirrorTextureD3D11(context->mirrorSrv);
	//context->mirrorSrv->Release();

	if (IsVRSystemInitialized) {
		IsVRSystemInitialized = false;
		vr::VR_Shutdown(); // Releases mirrorSrv
	}

	if (context->ctx11)
		context->ctx11->Release();
	if (context->dev11) {
		if (context->dev11->Release() != 0) {
			warn("win_openvr_deinit: device refcount not zero!");
		}
	}

	context->ctx11 = NULL;
	context->dev11 = NULL;
	context->tex = NULL;
	context->mirrorSrv = NULL;
	context->texCrop = NULL;

	context->device_width = 0;
	context->device_height = 0;
}

static const char *win_openvr_get_name(void *unused)
{
	//UNUSED_PARAMETER(unused);
	return "OpenVR Capture";
}

static void win_openvr_update(void *data, obs_data_t *settings)
{
	struct win_openvr *context = (win_openvr *)data;
	context->righteye = obs_data_get_bool(settings, "righteye");

	// if (context->righteye) { // L-R

	// } else { // R-L

	// }

	// zoom/scaling
	context->scale_factor = obs_data_get_double(settings, "scale_factor");
	// if (context->scale_factor < 1.00) {
	// 	context->scale_factor = 1.00;
	// }

	context->x_offset = (int)obs_data_get_int(settings, "x_offset");
	context->y_offset = (int)obs_data_get_int(settings, "y_offset");

	context->active_aspect_ratio = obs_data_get_double(settings, "aspect_ratio");

	if (context->active_aspect_ratio == -1.0) {
		context->ar_crop = false;
	} else {
		context->ar_crop = true;

		if (context->active_aspect_ratio == 0.0) {
			int custom_width = (int)obs_data_get_int(settings, "custom_aspect_width");
			int custom_height = (int)obs_data_get_int(settings, "custom_aspect_height");
			if (custom_width > 0 && custom_height > 0) {
				context->active_aspect_ratio = static_cast<double>(custom_width) / custom_height;
			} else {
				context->active_aspect_ratio = 16.0 / 9.0;
			}
		}
	}

	if (context->initialized) {
		win_openvr_deinit(data);
		win_openvr_init(data);
	}
}

static void win_openvr_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "righteye", true);
	obs_data_set_default_double(settings, "aspect_ratio", -1.0);
	obs_data_set_default_int(settings, "custom_aspect_width", 16);
	obs_data_set_default_int(settings, "custom_aspect_height", 9);
	obs_data_set_default_double(settings, "scale_factor", 1.0);
	obs_data_set_default_int(settings, "x_offset", 0);
	obs_data_set_default_int(settings, "y_offset", 0);
}

static uint32_t win_openvr_getwidth(void *data)
{
	struct win_openvr *context = (win_openvr *)data;
	return context->width;
}

static uint32_t win_openvr_getheight(void *data)
{
	struct win_openvr *context = (win_openvr *)data;
	return context->height;
}

static void win_openvr_show(void *data)
{
	win_openvr_init(data,
			true); // When showing do forced init without delay
}

static void win_openvr_hide(void *data)
{
	win_openvr_deinit(data);
}

static void *win_openvr_create(obs_data_t *settings, obs_source_t *source)
{
	struct win_openvr *context = (win_openvr *)bzalloc(sizeof(win_openvr));
	context->source = source;

	context->initialized = false;

	context->ctx11 = NULL;
	context->dev11 = NULL;
	context->tex = NULL;
	context->texture = NULL;
	context->texCrop = NULL;

	context->width = context->height = 100;

	context->active_aspect_ratio = 4.0 / 3.0;

	win_openvr_update(context, settings);
	return context;
}

static void win_openvr_destroy(void *data)
{
	struct win_openvr *context = (win_openvr *)data;

	win_openvr_deinit(data);
	bfree(context);
}

static void win_openvr_render(void *data, gs_effect_t *effect)
{
	struct win_openvr *context = (win_openvr *)data;

	if (context->active && !context->initialized) {
		// Active & want to render but not initialized - attempt to init
		win_openvr_init(data);
	}

	if (!context->texture || !context->active) {
		return;
	}

	// This step is required even without cropping as the full res mirror texture is in sRGB space
	D3D11_BOX poksi = {
		context->x,
		context->y,
		0,
		context->x + context->width,
		context->y + context->height,
		1,
	};
	context->ctx11->CopySubresourceRegion(context->texCrop, 0, 0, 0, 0, context->tex, 0, &poksi);
	context->ctx11->Flush();

	// Draw from OpenVR shared mirror texture
	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	while (gs_effect_loop(effect, "Draw")) {
		obs_source_draw(context->texture, 0, 0, 0, 0, false);
	}
}

static void win_openvr_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openvr *context = (win_openvr *)data;

	context->active = obs_source_active(context->source);

	if (context->initialized) {
		vr::VREvent_t e;

		if ((vr::VRSystem() != NULL) && (IsVRSystemInitialized)) {
			if (vr::VRSystem()->PollNextEvent(
				    &e, sizeof(vr::VREvent_t))) {
				if (e.eventType == vr::VREvent_Quit) {
					//vr::VRSystem()->AcknowledgeQuit_Exiting();
					//vr::VRSystem()->AcknowledgeQuit_UserPrompt();

					// Without this SteamVR will kill OBS process when it exits
					win_openvr_deinit(data);
				}
			}
		} else if (context->active) {
			context->initialized = false;
			win_openvr_init(data);
		}
	}
}

static bool button_reset_callback(obs_properties_t *props, obs_property_t *p,
				  void *data)
{
	struct win_openvr *context = (win_openvr *)data;

	if (GetTickCount64() - 2000 < context->lastCheckTick) {
		return false;
	}

	context->lastCheckTick = GetTickCount64();
	context->initialized = false;
	win_openvr_deinit(data);
	return false;
}

static bool ar_modd(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	double aspect_ratio = obs_data_get_double(settings, "aspect_ratio");

	bool custom_active = (aspect_ratio == 0.0);

	obs_property_t *custom_width = obs_properties_get(props, "custom_aspect_width");
	obs_property_t *custom_height = obs_properties_get(props, "custom_aspect_height");

	obs_property_set_visible(custom_width, custom_active);
	obs_property_set_visible(custom_height, custom_active);

	return true;
}

static obs_properties_t *win_openvr_properties(void *data)
{
	win_openvr *context = (win_openvr *)data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_bool(props, "righteye", obs_module_text("Right Eye"));

	// Preset aspect ratios
	p = obs_properties_add_list(props, "aspect_ratio", obs_module_text("Aspect Ratio"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(p, "Native", -1.0);
	obs_property_list_add_float(p, "16:9", 16.0 / 9.0);
	obs_property_list_add_float(p, "4:3", 4.0 / 3.0);
	obs_property_list_add_float(p, "Custom", 0.0);

	obs_property_set_modified_callback(p, ar_modd);

	p = obs_properties_add_int(props, "custom_aspect_width", obs_module_text("Ratio Width"), 1, 100, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, "custom_aspect_height", obs_module_text("Ratio Height"), 1, 100, 1);
	obs_property_set_visible(p, false);

	// Pan and zoom
	p = obs_properties_add_float_slider(props, "scale_factor", obs_module_text("Zoom"), 1.0, 5.0, 0.01);
	p = obs_properties_add_int(props, "x_offset", obs_module_text("Horizontal Offset"), -10000, 10000, 1);
	p = obs_properties_add_int(props, "y_offset", obs_module_text("Vertical Offset"), -10000, 10000, 1);

	p = obs_properties_add_button(props, "resetsteamvr", "Reinitialize OpenVR Source", button_reset_callback);

	obs_data_t *settings = obs_source_get_settings(context->source);
	ar_modd(props, NULL, settings);
	obs_data_release(settings);

	return props;
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-openvr", "en-US")

bool obs_module_load(void)
{
	obs_source_info info = {};
	info.id = "openvr_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = win_openvr_get_name;
	info.create = win_openvr_create;
	info.destroy = win_openvr_destroy;
	info.update = win_openvr_update;
	info.get_defaults = win_openvr_defaults;
	info.show = win_openvr_show;
	info.hide = win_openvr_hide;
	info.get_width = win_openvr_getwidth;
	info.get_height = win_openvr_getheight;
	info.video_render = win_openvr_render;
	info.video_tick = win_openvr_tick;
	info.get_properties = win_openvr_properties;
	obs_register_source(&info);
	return true;
}
