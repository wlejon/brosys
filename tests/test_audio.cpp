#include "brosys/audio.h"
#include "test_common.h"

#include <cassert>
#include <iostream>

int main() {
    init_test();
    std::cout << "[test_audio] Testing audio subsystem...\n";

    // 1. Live system query
    float master_vol = brosys::audio::get_master_volume();
    bool is_muted = brosys::audio::is_master_muted();
    std::cout << "  Live Master volume: " << (master_vol * 100.0f) << "%\n";
    std::cout << "  Live Master muted: " << (is_muted ? "Yes" : "No") << "\n";
    assert(master_vol >= 0.0f && master_vol <= 1.0f);

    auto outputs = brosys::audio::get_output_endpoints();
    auto inputs = brosys::audio::get_input_endpoints();
    std::cout << "  Output devices found: " << outputs.size() << "\n";
    for (const auto& ep : outputs) {
        std::cout << "    - Output: " << ep.name << " (Default: " << (ep.is_default ? "Yes" : "No") << ")\n";
    }
    std::cout << "  Input devices found: " << inputs.size() << "\n";
    for (const auto& ep : inputs) {
        std::cout << "    - Input: " << ep.name << " (Default: " << (ep.is_default ? "Yes" : "No") << ")\n";
    }

    // 2. Testing AudioManager with Mock Endpoints
    brosys::AudioManager mgr;
    assert(!mgr.is_mocked());

    std::vector<brosys::AudioEndpoint> mock_outs;
    brosys::AudioEndpoint spk;
    spk.id = "mock_spk_0";
    spk.name = "Realtek High Definition Speakers";
    spk.direction = brosys::EndpointDirection::Output;
    spk.is_default = true;
    spk.volume = 0.65f;
    spk.is_muted = false;
    mock_outs.push_back(spk);

    brosys::AudioEndpoint hdmi;
    hdmi.id = "mock_hdmi_1";
    hdmi.name = "HDMI Audio";
    hdmi.direction = brosys::EndpointDirection::Output;
    hdmi.is_default = false;
    hdmi.volume = 0.50f;
    hdmi.is_muted = true;
    mock_outs.push_back(hdmi);

    std::vector<brosys::AudioEndpoint> mock_ins;
    brosys::AudioEndpoint mic;
    mic.id = "mock_mic_0";
    mic.name = "USB Microphone";
    mic.direction = brosys::EndpointDirection::Input;
    mic.is_default = true;
    mic.volume = 0.80f;
    mic.is_muted = false;
    mock_ins.push_back(mic);

    mgr.set_mock_endpoints(mock_outs, mock_ins);
    assert(mgr.is_mocked());

    // Verify mock queries
    auto q_outs = mgr.get_output_endpoints();
    assert(q_outs.size() == 2);
    assert(q_outs[0].name == "Realtek High Definition Speakers");

    auto q_def_out = mgr.get_default_output();
    assert(q_def_out.has_value());
    assert(q_def_out->id == "mock_spk_0");

    auto q_def_in = mgr.get_default_input();
    assert(q_def_in.has_value());
    assert(q_def_in->id == "mock_mic_0");

    // Test volume set/get
    assert(mgr.set_master_volume(0.85f));
    assert(mgr.get_master_volume() == 0.85f);

    assert(!mgr.is_master_muted());
    assert(mgr.set_master_mute(true));
    assert(mgr.is_master_muted());
    assert(mgr.toggle_master_mute());
    assert(!mgr.is_master_muted());

    // Test endpoint-specific volume/mute
    assert(mgr.set_endpoint_volume("mock_hdmi_1", 0.90f));
    assert(mgr.set_endpoint_mute("mock_hdmi_1", false));

    auto updated_outs = mgr.get_output_endpoints();
    assert(updated_outs[1].volume == 0.90f);
    assert(updated_outs[1].is_muted == false);

    // Test Volume Notification Callback
    bool vol_cb_fired = false;
    mgr.register_volume_callback([&](const brosys::VolumeNotification& n) {
        if (n.volume == 0.42f) {
            vol_cb_fired = true;
        }
    });

    mgr.set_master_volume(0.42f);
    assert(vol_cb_fired);

    mgr.clear_mock_endpoints();
    assert(!mgr.is_mocked());

    std::cout << "[test_audio] PASSED\n";
    return 0;
}
