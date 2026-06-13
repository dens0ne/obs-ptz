/* Pan Tilt Zoom USB UVC implementation
	*
	* Copyright 2025 Fabio Ferrari <fabio.ferrar@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/

#include <qt-wrappers.hpp>
#include "ptz-device.hpp"
#include <cstddef>
#include <obs-data.h>
#include <obs-properties.h>
#include <obs.h>
#include <obs.hpp>
#include "ptz-usb-cam.hpp"

#ifdef __linux__
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

class V4L2Control : public PTZControl {
private:
	int fd;
	int query_ctrl(unsigned int i, long *pmin, long *pmax)
	{
		if (fd == -1)
			return false;
		struct v4l2_queryctrl queryctrl = {};
		queryctrl.id = i;
		if (ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl) == -1) {
			blog(LOG_ERROR, "VIDIOC_QUERYCTRL failed for axis %d", i);
			return -1;
		}
		*pmin = queryctrl.minimum;
		*pmax = queryctrl.maximum;
		return 0;
	}
	int set_ctrl(unsigned int i, long value)
	{
		if (fd == -1)
			return false;
		struct v4l2_control control = {};
		control.id = i;
		control.value = value;
		if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1) {
			blog(LOG_ERROR, "Failed to set PTZ %d value", i);
			return false;
		}
		return true;
	}
	int get_ctrl(unsigned int i)
	{
		if (fd == -1)
			return 0;
		struct v4l2_control control = {};
		control.id = i;
		if (ioctl(fd, VIDIOC_G_CTRL, &control) == -1) {
			blog(LOG_ERROR, "VIDIOC_G_CTRL failed for axis %d", i);
			return 0;
		}
		return control.value;
	}

public:
	V4L2Control(const std::string &device)
	{
		device_path = device;
		fd = open(device_path.c_str(), O_RDWR);
		if (fd == -1) {
			blog(LOG_ERROR, "Failed to open V4L2 device: %s", device_path.c_str());
			return;
		}
		query_ctrl(V4L2_CID_PAN_ABSOLUTE, &min.pan, &max.pan);
		query_ctrl(V4L2_CID_TILT_ABSOLUTE, &min.tilt, &max.tilt);
		query_ctrl(V4L2_CID_ZOOM_ABSOLUTE, &min.zoom, &max.zoom);
		query_ctrl(V4L2_CID_FOCUS_ABSOLUTE, &min.focus, &max.focus);
		now_pos.pan = static_cast<double>(get_ctrl(V4L2_CID_PAN_ABSOLUTE)) / max.pan;
		now_pos.tilt = static_cast<double>(get_ctrl(V4L2_CID_TILT_ABSOLUTE)) / max.tilt;
		now_pos.zoom = static_cast<double>(get_ctrl(V4L2_CID_ZOOM_ABSOLUTE)) / max.zoom;
		now_pos.focus = static_cast<double>(get_ctrl(V4L2_CID_FOCUS_ABSOLUTE)) / max.focus;
		now_pos.focusAuto = get_ctrl(V4L2_CID_FOCUS_AUTO);
	}
	bool internal_pan(long value) override { return set_ctrl(V4L2_CID_PAN_ABSOLUTE, value); }
	bool internal_tilt(long value) override { return set_ctrl(V4L2_CID_TILT_ABSOLUTE, value); }
	bool internal_zoom(long value) override { return set_ctrl(V4L2_CID_ZOOM_ABSOLUTE, value); }
	bool internal_focus(bool auto_focus, long value) override
	{
		if (auto_focus) {
			return set_ctrl(V4L2_CID_FOCUS_AUTO, 1);
		} else {
			if (get_ctrl(V4L2_CID_FOCUS_AUTO) != 0) {
				set_ctrl(V4L2_CID_FOCUS_AUTO, 0);
			}
			return set_ctrl(V4L2_CID_FOCUS_ABSOLUTE, value);
		}
	}
	~V4L2Control() override
	{
		if (fd == -1)
			return;
		close(fd);
	}
	bool isValid() const override { return fd != -1; }
};
#endif

#ifdef _WIN32
#include <dshow.h>
#pragma comment(lib, "strmiids.lib") // Linka com DirectShow no Windows

class DirectShowControl : public PTZControl {
private:
	IBaseFilter *filter_ = nullptr;
	IAMCameraControl *cam_control_ = nullptr;
	long last_focus = 0;

public:
	DirectShowControl(const std::string &device)
	{
		device_path = device;
		QString decoded_path = QString::fromStdString(device_path);
		int colon_pos = decoded_path.indexOf(':');
		if (colon_pos != -1) {
			decoded_path = decoded_path.mid(colon_pos + 1);
		}
		decoded_path = decoded_path.split("#22").join("#");
		decoded_path = decoded_path.split("#3A").join(":");
		std::string decoded_std_path = decoded_path.toStdString();
		// blog(LOG_INFO, "PTZ-USB-CAM Device: %s", decoded_std_path.c_str());

		HRESULT hr = CoInitialize(nullptr);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
			blog(LOG_ERROR, "Failed to initialize COM: %ld", hr);
			return;
		}

		ICreateDevEnum *dev_enum = nullptr;
		hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
				      (void **)&dev_enum);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to create device enumerator: %ld", hr);
			return;
		}

		IEnumMoniker *enum_moniker = nullptr;
		hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_moniker, 0);
		if (FAILED(hr) || !enum_moniker) {
			blog(LOG_ERROR, "Failed to enumerate video devices: %ld", hr);
			dev_enum->Release();
			return;
		}

		IMoniker *moniker = nullptr;
		while (enum_moniker->Next(1, &moniker, nullptr) == S_OK) {
			IPropertyBag *prop_bag = nullptr;
			hr = moniker->BindToStorage(0, 0, IID_IPropertyBag, (void **)&prop_bag);
			if (FAILED(hr)) {
				moniker->Release();
				continue;
			}

			VARIANT var_name;
			VariantInit(&var_name);
			hr = prop_bag->Read(L"DevicePath", &var_name, 0);
			if (SUCCEEDED(hr)) {
				std::wstring w_device_path(var_name.bstrVal);
				std::string device_path1(w_device_path.begin(), w_device_path.end());
				if (device_path1 == decoded_std_path) {
					hr = moniker->BindToObject(0, 0, IID_IBaseFilter, (void **)&filter_);
					if (SUCCEEDED(hr)) {
						hr = filter_->QueryInterface(IID_IAMCameraControl,
									     (void **)&cam_control_);
						if (FAILED(hr)) {
							blog(LOG_ERROR, "Failed to get IAMCameraControl: %ld", hr);
							filter_->Release();
							filter_ = nullptr;
						}
					}
				}
				VariantClear(&var_name);
			}

			prop_bag->Release();
			moniker->Release();

			if (cam_control_)
				break;
		}

		enum_moniker->Release();
		dev_enum->Release();

		if (cam_control_ == nullptr) {
			return;
		}
		// blog(LOG_INFO, "Obtained DirectShow filter for device: %s", device_name.c_str());
		long step, default_value, flags;
		hr = cam_control_->GetRange(CameraControl_Pan, &min.pan, &max.pan, &step, &default_value, &flags);
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Tilt, &min.tilt, &max.tilt, &step, &default_value,
						    &flags);
		}
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Zoom, &min.zoom, &max.zoom, &step, &default_value,
						    &flags);
		}
		if (!FAILED(hr)) {
			hr = cam_control_->GetRange(CameraControl_Focus, &min.focus, &max.focus, &step, &default_value,
						    &flags);
		}
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to get ranges: %ld", hr);
			return;
		}
		long pan, tilt, zoom, focus;
		cam_control_->Get(CameraControl_Pan, &pan, &flags);
		now_pos.pan = static_cast<double>(pan) / max.pan;
		cam_control_->Get(CameraControl_Tilt, &tilt, &flags);
		now_pos.tilt = static_cast<double>(tilt) / max.tilt;
		cam_control_->Get(CameraControl_Zoom, &zoom, &flags);
		now_pos.zoom = static_cast<double>(zoom) / max.zoom;
		cam_control_->Get(CameraControl_Focus, &focus, &flags);
		now_pos.focus = static_cast<double>(focus) / max.focus;
		now_pos.focusAuto = (flags & CameraControl_Flags_Auto) != 0;
	}
	bool internal_pan(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Pan, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Pan: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_tilt(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Tilt, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Tilt: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_zoom(long value) override
	{
		if (!cam_control_)
			return false;
		HRESULT hr = cam_control_->Set(CameraControl_Zoom, value, CameraControl_Flags_Manual);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set Zoom: %ld (mapped value: %ld)", hr, value);
			return false;
		}
		return true;
	}
	bool internal_focus(bool auto_focus, long focus) override
	{
		if (!cam_control_)
			return false;
		auto focus_flag = auto_focus ? CameraControl_Flags_Auto : CameraControl_Flags_Manual;
		HRESULT hr;
		hr = cam_control_->Set(CameraControl_Focus, focus, focus_flag);
		if (FAILED(hr)) {
			blog(LOG_ERROR, "Failed to set AutoFocus: %ld", hr);
			return false;
		}
		return true;
	}
	~DirectShowControl() override
	{
		if (cam_control_) {
			cam_control_->Release();
		}
		if (filter_) {
			filter_->Release();
		}
	}
	double readHardwareTilt() const override
	{
		if (!cam_control_)
			return now_pos.tilt;
		long tilt, flags;
		if (SUCCEEDED(cam_control_->Get(CameraControl_Tilt, &tilt, &flags)))
			return static_cast<double>(tilt) / max.tilt;
		return now_pos.tilt;
	}
	double readHardwarePan() const override
	{
		if (!cam_control_)
			return now_pos.pan;
		long pan, flags;
		if (SUCCEEDED(cam_control_->Get(CameraControl_Pan, &pan, &flags)))
			return static_cast<double>(pan) / max.pan;
		return now_pos.pan;
	}
	bool isValid() const override { return cam_control_ != nullptr; }
};
#endif

void PTZUSBCam::ptz_tick_callback(void *param, float seconds)
{
	PTZUSBCam *cam = static_cast<PTZUSBCam *>(param);
	cam->ptz_tick(seconds);
}

PTZUSBCam::PTZUSBCam(OBSData config) : PTZDevice(config)
{
	getDefaults(config);
	update(config);
	obs_add_tick_callback(ptz_tick_callback, this);
}

PTZUSBCam::~PTZUSBCam()
{
	obs_remove_tick_callback(ptz_tick_callback, this);
}

QString PTZUSBCam::description()
{
	return QString(obs_module_text("PTZ.UVC.Name"));
}

void PTZUSBCam::getDefaults(OBSData defaults) const
{
	PTZDevice::getDefaults(defaults);
	obs_data_set_default_double(defaults, "preset_transition_speed", 0.0);
}

void PTZUSBCam::update(OBSData config)
{
	PTZDevice::update(config);
	preset_transition_speed = obs_data_get_double(config, "preset_transition_speed");
	OBSDataArrayAutoRelease presetArray = obs_data_get_array(config, "presets_memory");
	size_t count = obs_data_array_count(presetArray);
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease preset = obs_data_array_item(presetArray, i);
		int p_id = static_cast<int>(obs_data_get_int(preset, "preset_id"));
		PtzUsbCamPos p = PtzUsbCamPos();
		p.pan = obs_data_get_double(preset, "pan");
		p.tilt = obs_data_get_double(preset, "tilt");
		p.zoom = obs_data_get_double(preset, "zoom");
		p.focusAuto = obs_data_get_bool(preset, "focusauto");
		p.focus = obs_data_get_double(preset, "focus");
		// p.whitebalAuto = obs_data_get_bool(preset, "whitebalauto");
		// p.temperature = obs_data_get_double(preset, "temperature");
		presets[p_id] = p;
	}
}

void PTZUSBCam::save(OBSData config) const
{
	PTZDevice::save(config);
	OBSDataArrayAutoRelease presetArray = obs_data_array_create();
	for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
		const PtzUsbCamPos &preset = it.value();
		OBSDataAutoRelease presetData = obs_data_create();
		obs_data_set_double(presetData, "preset_id", it.key());
		obs_data_set_double(presetData, "pan", preset.pan);
		obs_data_set_double(presetData, "tilt", preset.tilt);
		obs_data_set_double(presetData, "zoom", preset.zoom);
		obs_data_set_bool(presetData, "focusauto", preset.focusAuto);
		obs_data_set_double(presetData, "focus", preset.focus);
		// obs_data_set_bool(presetData, "whitebalauto",
		// 		  preset.whitebalAuto);
		// obs_data_set_double(presetData, "temperature", preset.temperature);
		obs_data_array_push_back(presetArray, presetData);
	}
	obs_data_set_array(config, "presets_memory", presetArray);
	obs_data_set_double(config, "preset_transition_speed", preset_transition_speed);
}

obs_properties_t *PTZUSBCam::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_properties_remove_by_name(ptz_props, "interface");

	obs_property_t *general_prop = obs_properties_get(ptz_props, "general");
	obs_properties_t *general = obs_property_group_content(general_prop);
	obs_property_t *speed_list =
		obs_properties_add_list(general, "preset_transition_speed",
					obs_module_text("PTZ.UVC.PresetTransitionSpeed"),
					OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(speed_list, obs_module_text("PTZ.UVC.PresetTransitionSpeed.Instant"), 0.0);
	obs_property_list_add_float(speed_list, obs_module_text("PTZ.UVC.PresetTransitionSpeed.Slow"), 0.020);
	obs_property_list_add_float(speed_list, obs_module_text("PTZ.UVC.PresetTransitionSpeed.Medium"), 0.050);
	obs_property_list_add_float(speed_list, obs_module_text("PTZ.UVC.PresetTransitionSpeed.Fast"), 0.100);

	return ptz_props;
}

void PTZUSBCam::do_update()
{
	pantilt_changed = false;
	zoom_changed = false;
	focus_changed = false;
}

PTZControl *PTZUSBCam::get_ptz_control()
{
	std::string video_device_id = "";
	OBSSourceAutoRelease src = obs_get_source_by_name(QT_TO_UTF8(objectName()));
	if (src) {
		OBSDataAutoRelease psettings = obs_source_get_settings(src);
		if (psettings) {
#ifdef _WIN32
			video_device_id = obs_data_get_string(psettings, "video_device_id");
#else
			video_device_id = obs_data_get_string(psettings, "device_id");
#endif
		}
	}

	// already have the device, and it didn't change: nothing to do
	if (ptz_control_ != nullptr && ptz_control_->isValid() && ptz_control_->getDevicePath() == video_device_id) {
		return ptz_control_;
	}
	blog(LOG_INFO, "Switching PTZ USBUVC device from %s to %s",
	     ptz_control_ == nullptr ? "null" : ptz_control_->getDevicePath().c_str(),
	     video_device_id.empty() ? "null" : video_device_id.c_str());
	if (ptz_control_ != nullptr) {
		delete ptz_control_;
		ptz_control_ = nullptr;
	}
	if (video_device_id.empty()) {
		return nullptr;
	}
#ifdef __linux__
	ptz_control_ = new V4L2Control(video_device_id);
#endif
#ifdef _WIN32
	ptz_control_ = new DirectShowControl(video_device_id);
#endif
	if (ptz_control_ == nullptr || !ptz_control_->isValid()) {
		return nullptr;
	}
	return ptz_control_;
}

void PTZUSBCam::ptz_tick(float seconds)
{
	tick_elapsed += seconds;

	if (preset_transitioning) {
		if (std::abs(pan_speed) > 0.001 || std::abs(tilt_speed) > 0.001 || std::abs(zoom_speed) > 0.001) {
			blog(LOG_INFO, "[obs-ptz] transition cancelled by joystick input");
			preset_transitioning = false;
			// fall through to regular joystick handling below
		} else {
			double frame_time = static_cast<double>(seconds) < 0.05 ? static_cast<double>(seconds) : 0.05;
			transition_elapsed += frame_time;
			transition_cmd_elapsed += frame_time;
			transition_log_elapsed += frame_time;
			transition_hw_read_elapsed += frame_time;
			double t = transition_elapsed / preset_transition_duration;
			if (t > 1.0)
				t = 1.0;

			// Send camera commands at ~30 Hz to avoid overwhelming the
			// camera's command buffer. Always send the final frame.
			bool send_cmd = (transition_cmd_elapsed >= 0.033) || (t >= 1.0);
			if (send_cmd) {
				transition_cmd_elapsed = 0.0;
				auto ptzctrl = get_ptz_control();
				if (ptzctrl) {
					// Refresh hardware position at 5 Hz to minimise USB bandwidth pressure.
					// Reads at every 30 Hz command cycle caused contention with the video stream.
					if (transition_hw_read_elapsed >= 0.2) {
						transition_hw_read_elapsed = 0.0;
						hw_pan_cached = ptzctrl->readHardwarePan();
						hw_tilt_cached = ptzctrl->readHardwareTilt();
					}
					double hw_pan = hw_pan_cached;
					double hw_tilt = hw_tilt_cached;

					// Time-based desired position
					double desired_pan = preset_start.pan + t * (preset_target.pan - preset_start.pan);
					double desired_tilt =
						preset_start.tilt + t * (preset_target.tilt - preset_start.tilt);

					// Cap commanded position to no more than max_lead ahead of hardware.
					// If the camera servo stalls, we slow down with it so there is no
					// large catch-up sprint when it unfreezes.
					const double max_lead = 0.05;
					double new_pan = desired_pan;
					if (preset_target.pan < preset_start.pan)
						new_pan = desired_pan > hw_pan - max_lead ? desired_pan : hw_pan - max_lead;
					else if (preset_target.pan > preset_start.pan)
						new_pan = desired_pan < hw_pan + max_lead ? desired_pan : hw_pan + max_lead;

					double new_tilt = desired_tilt;
					if (preset_target.tilt < preset_start.tilt)
						new_tilt = desired_tilt > hw_tilt - max_lead ? desired_tilt
										      : hw_tilt - max_lead;
					else if (preset_target.tilt > preset_start.tilt)
						new_tilt = desired_tilt < hw_tilt + max_lead ? desired_tilt
										      : hw_tilt + max_lead;

					ptzctrl->pan(new_pan);
					ptzctrl->tilt(new_tilt);
					if (preset_target.zoom != preset_start.zoom) {
						double new_zoom =
							preset_start.zoom + t * (preset_target.zoom - preset_start.zoom);
						ptzctrl->zoom(new_zoom);
					}

					// Log once per second for diagnostics
					if (transition_log_elapsed >= 1.0) {
						transition_log_elapsed = 0.0;
						blog(LOG_INFO,
						     "[obs-ptz] t=%.2f cmd_pan=%.3f hw_pan=%.3f pan_lag=%.3f | cmd_tilt=%.3f hw_tilt=%.3f tilt_lag=%.3f",
						     t, new_pan, hw_pan, hw_pan - new_pan,
						     new_tilt, hw_tilt, hw_tilt - new_tilt);
					}

					if (t >= 1.0) {
						ptzctrl->setAutoFocus(preset_target.focusAuto);
						if (!preset_target.focusAuto)
							ptzctrl->focus(preset_target.focus);
						preset_transitioning = false;
						blog(LOG_INFO, "[obs-ptz] transition complete after %.1fs",
						     transition_elapsed);
					}
				}
			}
			tick_elapsed = 0.0f;
			return;
		}
	}

	if (tick_elapsed < 0.03f)
		return;

	if (pan_speed != 0.0 || tilt_speed != 0.0) {
		pantilt_rel(pan_speed * tick_elapsed, tilt_speed * tick_elapsed);
	}
	if (zoom_speed != 0.0) {
		auto ptzctrl = get_ptz_control();
		if (ptzctrl) {
			zoom_abs(ptzctrl->getZoom() + zoom_speed * tick_elapsed);
		}
	}
	if (focus_speed != 0.0) {
		auto ptzctrl = get_ptz_control();
		if (ptzctrl) {
			focus_abs(ptzctrl->getFocus() + focus_speed * tick_elapsed);
		}
	}
	tick_elapsed = 0.0f;
}

void PTZUSBCam::pantilt_abs(double pan, double tilt)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->pan(pan);
	ptzctrl->tilt(tilt);
}

void PTZUSBCam::pantilt_rel(double pan, double tilt)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	pantilt_abs(ptzctrl->getPan() + pan, ptzctrl->getTilt() + tilt);
}

void PTZUSBCam::pantilt_home()
{
	pantilt_abs(0, 0);
}

void PTZUSBCam::zoom_abs(double pos)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->zoom(pos);
}

void PTZUSBCam::focus_abs(double pos)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->focus(pos);
}

void PTZUSBCam::set_autofocus(bool enabled)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	ptzctrl->setAutoFocus(enabled);
}

void PTZUSBCam::memory_reset(int i)
{
	if (!presets.contains(i))
		return;
	presets.remove(i);
}

void PTZUSBCam::memory_set(int i)
{
	auto ptzctrl = get_ptz_control();
	if (!ptzctrl)
		return;
	presets[i] = ptzctrl->getPosition();
}

void PTZUSBCam::memory_recall(int i)
{
	if (!presets.contains(i))
		return;
	preset_target = presets[i];
	if (preset_transition_speed > 0.0) {
		auto ptzctrl = get_ptz_control();
		if (ptzctrl) {
			preset_start = ptzctrl->getPosition();
			hw_pan_cached = ptzctrl->readHardwarePan();
			hw_tilt_cached = ptzctrl->readHardwareTilt();
		} else {
			blog(LOG_WARNING, "[obs-ptz] no PTZ control at recall time, cancelling transition");
			preset_start = preset_target;
			hw_pan_cached = preset_target.pan;
			hw_tilt_cached = preset_target.tilt;
		}
		double pan_dist = preset_target.pan - preset_start.pan;
		double tilt_dist = preset_target.tilt - preset_start.tilt;
		if (pan_dist < 0.0)
			pan_dist = -pan_dist;
		if (tilt_dist < 0.0)
			tilt_dist = -tilt_dist;
		double distance = pan_dist > tilt_dist ? pan_dist : tilt_dist;
		if (distance < 0.001) {
			pantilt_abs(preset_target.pan, preset_target.tilt);
			zoom_abs(preset_target.zoom);
			set_autofocus(preset_target.focusAuto);
			if (!preset_target.focusAuto)
				focus_abs(preset_target.focus);
			return;
		}
		preset_transition_duration = distance / preset_transition_speed;
		transition_elapsed = 0.0;
		transition_cmd_elapsed = 0.0;
		transition_log_elapsed = 0.0;
		transition_hw_read_elapsed = 0.0;
		preset_transitioning = true;
		blog(LOG_INFO, "[obs-ptz] transition start: pan %.3f->%.3f tilt %.3f->%.3f zoom %.3f->%.3f dur=%.1fs",
		     preset_start.pan, preset_target.pan, preset_start.tilt, preset_target.tilt,
		     preset_start.zoom, preset_target.zoom, preset_transition_duration);
		return;
	}
	pantilt_abs(preset_target.pan, preset_target.tilt);
	zoom_abs(preset_target.zoom);
	set_autofocus(preset_target.focusAuto);
	if (!preset_target.focusAuto)
		focus_abs(preset_target.focus);
}
