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
#include <stdint.h>
#include <algorithm>
#include <vector>
#include <chrono>
#include "headers/openvr.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "lib/win64/openvr_api.lib")

static bool init_inprog = false;
static bool IsVRSystemInitialized = false;

std::chrono::steady_clock::time_point last_init_time = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point last_init_timeBUFFER = std::chrono::steady_clock::now();
static constexpr std::chrono::milliseconds retry_delay{8}; // update at ~120Hz
static constexpr std::chrono::milliseconds retry_delayBUFFER{500}; // init at 2Hz

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

	uint32_t lastFrame;

	gs_texture_t *texture;
	ID3D11Device *dev11;
	ID3D11DeviceContext *ctx11;
	ID3D11Resource *tex;
	ID3D11ShaderResourceView *mirrorSrv;

	IDXGIResource *res;

	ID3D11Texture2D *texCrop;

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

// Helper to safely release COM objects
static void safe_release(IUnknown **theobject) {
	if (theobject && *theobject) {
		(*theobject)->Release();
		*theobject = nullptr;
	}
}

// Helper to destroy OBS texture
static void destroy_obs_texture(gs_texture_t **texture) {
	if (texture && *texture) {
		obs_enter_graphics();
		gs_texture_destroy(*texture);
		obs_leave_graphics();
		*texture = nullptr;
	}
}

ID3D11Device *shared_device = nullptr;
ID3D11DeviceContext *shared_context = nullptr;

/// This is the messiest code i have written in my life, one day i will fix it but that day is not today.
static void win_openvr_init(void *data, bool forced = true)
{
	win_openvr *context = (win_openvr *)data;

	if (context->initialized || init_inprog) {
		return;
	}
	
	auto now = std::chrono::steady_clock::now();
	if (now - last_init_time < retry_delay) {
		return;
	}
	last_init_time = now;

	init_inprog = true;

	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Background);
	if (err != vr::VRInitError_None) {
		warn("win_openvr_init: OpenVR initialization failed! %s", vr::VR_GetVRInitErrorAsEnglishDescription(err));
		init_inprog = false;
		return;
	}

	if (!shared_device) {
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &shared_device, nullptr, &shared_context);
		if (FAILED(hr)) {
			warn("win_openvr_init: SHARED D3D11CreateDevice failed");
			init_inprog = false;
			vr::VR_Shutdown();
			return;
		}
	}

	safe_release((IUnknown**)&context->texCrop);
	safe_release((IUnknown**)&context->tex);

	context->dev11 = shared_device;
	context->ctx11 = shared_context;
	context->dev11->AddRef();
	context->ctx11->AddRef();

	IsVRSystemInitialized = true;

	if (!vr::VRCompositor()) {
		warn("win_openvr_show: VR Compositor not found");
		init_inprog = false;
		vr::VR_Shutdown();
		return;
	}

	vr::EVRCompositorError composError = vr::VRCompositor()->GetMirrorTextureD3D11(context->righteye ? vr::Eye_Right : vr::Eye_Left, context->dev11, (void **)&context->mirrorSrv);

	context->mirrorSrv->GetResource(&context->tex);
	if (context->tex) {
		D3D11_TEXTURE2D_DESC desc = {};
		ID3D11Texture2D *tex2D = nullptr;
		context->tex->QueryInterface<ID3D11Texture2D>(&tex2D);
		if (tex2D) {
			tex2D->GetDesc(&desc);
			context->device_width = desc.Width;
			context->device_height = desc.Height;

			// Pan and zoom
			int x = 0, y = 0;

			double scale_factor = context->scale_factor < 1.0 ? 1.0 : context->scale_factor;
			unsigned int scaled_width = static_cast<unsigned int>(context->device_width / scale_factor);
			unsigned int scaled_height = static_cast<unsigned int>(context->device_height / scale_factor);
			context->width = scaled_width;
			context->height = scaled_height;

			if (context->ar_crop) {
				double input_aspect_ratio = static_cast<double>(context->width) / context->height;
				double active_aspect_ratio = context->active_aspect_ratio;
				if (input_aspect_ratio > active_aspect_ratio) {
					context->width = static_cast<unsigned int>(context->height * active_aspect_ratio);
				} else if (input_aspect_ratio < active_aspect_ratio) {
					context->height = static_cast<unsigned int>(context->width / active_aspect_ratio);
				}
			}

			int x_offset = context->x_offset;
			int y_offset = context->y_offset;
			if (!context->righteye) {
				x_offset = -x_offset;
				x = context->device_width - scaled_width;
			}
			x += x_offset;
			y += y_offset;
			if (x + context->width > context->device_width) x = context->device_width - context->width;
			if (y + context->height > context->device_height) y = context->device_height - context->height;

			x = std::max(0, x);
			y = std::max(0, y);
			context->x = x;
			context->y = y;

			desc.Width = context->width;
			desc.Height = context->height;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

			HRESULT hr = context->dev11->CreateTexture2D(&desc, nullptr, &context->texCrop);
			if (FAILED(hr)) {
				warn("win_openvr_show: CreateTexture2D failed");
				init_inprog = false;
				vr::VR_Shutdown();
				safe_release((IUnknown**)&tex2D);
				return;
			}

			HRESULT hrRes = context->texCrop->QueryInterface(__uuidof(IDXGIResource), (void **)&context->res);
			if (FAILED(hrRes)) {
				warn("win_openvr_show: QueryInterface failed");
				init_inprog = false;
				vr::VR_Shutdown();
				safe_release((IUnknown**)&tex2D);
				safe_release((IUnknown**)&context->texCrop);
				return;
			}
			HANDLE handle = nullptr;
			HRESULT hrHandle = context->res->GetSharedHandle(&handle);
			if (FAILED(hrHandle)) {
				warn("win_openvr_show: GetSharedHandle failed");
				init_inprog = false;
				vr::VR_Shutdown();
				safe_release((IUnknown**)&context->res);
				safe_release((IUnknown**)&tex2D);
				safe_release((IUnknown**)&context->texCrop);
				return;
			}
			safe_release((IUnknown**)&context->res);

			uint32_t GShandle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
			destroy_obs_texture(&context->texture);
			obs_enter_graphics();
			context->texture = gs_texture_open_shared(GShandle);
			obs_leave_graphics();
			safe_release((IUnknown**)&tex2D);
		}
	}
	context->initialized = true;
	context->lastFrame = 0;
	init_inprog = false;
}

static void win_openvr_init1(void *data, bool forced = true) {
	win_openvr *context = (win_openvr *)data;

	if (context->initialized || init_inprog)
		return;

	auto now = std::chrono::steady_clock::now();
	if (now - last_init_timeBUFFER < retry_delayBUFFER) {
		return;
	}
	last_init_timeBUFFER = now;

	win_openvr_init(data, forced);
}

static void win_openvr_deinit(void *data)
{
	win_openvr *context = (win_openvr *)data;

	if (context->texture) destroy_obs_texture(&context->texture);
	if (context->texCrop) safe_release((IUnknown**)&context->texCrop);
	if (context->tex) safe_release((IUnknown**)&context->tex);
	if (context->mirrorSrv) safe_release((IUnknown**)&context->mirrorSrv);
	if (context->res) safe_release((IUnknown**)&context->res);
//	if (context->ctx11) safe_release((IUnknown**)&context->ctx11);
//	if (context->dev11) safe_release((IUnknown**)&context->dev11);

	vr::VR_Shutdown();

	context->initialized = false;
	init_inprog = false;
}

static const char *win_openvr_get_name(void *unused)
{
	return "OpenVR Capture";
}

static void win_openvr_update(void *data, obs_data_t *settings)
{
	struct win_openvr *context = (win_openvr *)data;

	context->righteye = obs_data_get_bool(settings, "righteye");

	// zoom/scaling
	context->scale_factor = obs_data_get_double(settings, "scale_factor");

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
		context->initialized = false; // Force re-init
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
	win_openvr_init1(data, true); // When showing do forced init without delay
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

//	context->ctx11 = nullptr;
//	context->dev11 = nullptr;
	context->tex = nullptr;
	context->texture = nullptr;
	context->texCrop = nullptr;
	context->mirrorSrv = nullptr;

	context->width = context->height = 100;

	context->active_aspect_ratio = 16.0 / 9.0;

	win_openvr_update(context, settings);
	return context;
}

static void win_openvr_destroy(void *data)
{
	struct win_openvr *context = (win_openvr *)data;

	win_openvr_deinit(data);
//	safe_release((IUnknown**)&shared_device);
//	safe_release((IUnknown**)&shared_context);
	bfree(context);
}

static void win_openvr_render(void *data, gs_effect_t *effect)
{
	win_openvr *context = (win_openvr *)data;

	if (!context->active) {
		return;
	}

	if (!context->initialized) {
		// Active & want to render but not initialized - attempt to init
		win_openvr_init1(data);
	}

	if (vr::VRCompositor()) {
		vr::Compositor_FrameTiming frameTiming = {};
		frameTiming.m_nSize = sizeof(vr::Compositor_FrameTiming);
		if (vr::VRCompositor()->GetFrameTiming(&frameTiming, 0)) {
			if (frameTiming.m_nFrameIndex != context->lastFrame) {
				if (context->texCrop && context->tex) {
					D3D11_BOX poksi = {context->x, context->y, 0, context->x + context->width, context->y + context->height, 1};
					context->ctx11->CopySubresourceRegion(context->texCrop, 0, 0, 0, 0, context->tex, 0, &poksi);
					context->ctx11->Flush();
					context->lastFrame = frameTiming.m_nFrameIndex;
				}
			}
		}
	}

	effect = obs_get_base_effect(OBS_EFFECT_OPAQUE);

	if (context->texture) {
		while (gs_effect_loop(effect, "Draw")) {
			obs_source_draw(context->texture, 0, 0, 0, 0, false);
		}
	}
}

static void win_openvr_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct win_openvr *context = (win_openvr *)data;

	context->active = obs_source_showing(context->source);

	vr::VREvent_t e;
	if (vr::VRSystem() != NULL) {
		if (vr::VRSystem()->PollNextEvent(&e, sizeof(vr::VREvent_t))) {
			if (e.eventType == vr::VREvent_Quit) {
				// Without this SteamVR will kill OBS process when it exits
				win_openvr_deinit(data);
			}
		}
	}

	if (!context->initialized && context->active) {
		win_openvr_init1(data);
	}
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
