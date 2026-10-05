// The macOS tray host: role None.
//
// Menu-bar extras are NSStatusItems that every application draws into the
// system menu bar itself (through the window server / Control Center). There
// is no protocol by which they reach another process, so nothing can be
// hosted; TrayMode does not change that. (Third-party menu-bar managers read
// the extras' accessibility elements and screenshots: scraping, not
// hosting, and it needs Accessibility and Screen Recording permission.)
//
// Deliberately not done: an NSStatusItem of the host's own. That is a tray
// *client*, which bro already gets portably from SDL3 (SDL_CreateTray) on the
// main thread AppKit requires, rather than from a host-side service.
#include "brosys/tray.h"

namespace brosys {

namespace {

constexpr const char* kDetail =
    "macOS has no tray protocol: menu-bar extras are NSStatusItems each application draws into the system menu bar "
    "itself, and none of them can be received or hosted by another process";

class MacTrayHost final : public TrayHost {
public:
    MacTrayHost() { events_.push(TrayHostStatus{TrayRole::None, kDetail}); }

    TrayEventQueue& events() override { return events_; }
    TrayHostStatus status() const override { return {TrayRole::None, kDetail}; }
    std::vector<TrayItem> items() const override { return {}; }

    Result activate(const std::string&, int32_t, int32_t) override { return none(); }
    Result secondary_activate(const std::string&, int32_t, int32_t) override { return none(); }
    Result context_menu(const std::string&, int32_t, int32_t) override { return none(); }
    Result scroll(const std::string&, int32_t, ScrollOrientation) override { return none(); }
    std::optional<MenuItem> menu(const std::string&) const override { return std::nullopt; }
    Result menu_about_to_show(const std::string&, int32_t) override { return none(); }
    Result menu_event(const std::string&, int32_t, MenuEventType) override { return none(); }
    Result set_item_rect(const std::string&, const Rect32&) override { return none(); }

private:
    static Result none() { return Result::failure(std::string("not hosting the tray: ") + kDetail); }

    TrayEventQueue events_;
};

}  // namespace

std::unique_ptr<TrayHost> TrayHost::create(const TrayConfig& config, std::string* error) {
    if (config.mode == TrayMode::Shell) {
        if (error) *error = std::string("TrayMode::Shell: ") + kDetail;
        return nullptr;
    }
    return std::make_unique<MacTrayHost>();
}

}  // namespace brosys
