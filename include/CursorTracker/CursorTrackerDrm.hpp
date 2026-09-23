#pragma once

#include "CursorTracker.hpp"

#include <vector>

struct wl_display;

namespace gsr {
    class CursorTrackerDrm : public CursorTracker {
    public:
        CursorTrackerDrm(struct wl_display *wayland_dpy);
        CursorTrackerDrm(const CursorTrackerDrm&) = delete;
        CursorTrackerDrm& operator=(const CursorTrackerDrm&) = delete;
        ~CursorTrackerDrm();

        void update() override;
        std::optional<CursorInfo> get_latest_cursor_info() override;
    private:
        // Returns true if the cursor was found on the gpu
        bool update_gpu(int drm_fd);
    private:
        std::vector<int> drm_fds; // One for every gpu, the monitor that the cursor is on can be connected to any gpu
        int latest_drm_fd = -1; // The gpu that the monitor that the cursor is on is connected to
        mgl::vec2i latest_cursor_position; // Position of the cursor within the monitor
        int latest_crtc_id = -1;
        struct wl_display *wayland_dpy = nullptr;
    };
}