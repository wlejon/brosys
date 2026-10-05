#include "win/tray_icon.h"

#include <vector>

namespace brosys::win::tray {

namespace {

// Reads `bitmap` as top-down 32bpp BGRA rows (`height` may be a part of a
// taller bitmap: only the first `height` rows are returned).
bool read_bits(HDC dc, HBITMAP bitmap, int width, int height, std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    return GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), out.data(), &bi, DIB_RGB_COLORS) ==
           static_cast<int>(height);
}

struct IconBitmaps {
    ICONINFO info{};
    ~IconBitmaps() {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
};

}  // namespace

std::optional<Image> icon_to_image(HICON icon) {
    if (!icon) return std::nullopt;
    IconBitmaps bm;
    if (!GetIconInfo(icon, &bm.info) || !bm.info.hbmMask) return std::nullopt;

    BITMAP mask{};
    if (!GetObjectW(bm.info.hbmMask, sizeof mask, &mask)) return std::nullopt;
    const bool monochrome = bm.info.hbmColor == nullptr;
    const int width = mask.bmWidth;
    const int height = monochrome ? mask.bmHeight / 2 : mask.bmHeight;
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return std::nullopt;

    HDC dc = GetDC(nullptr);
    if (!dc) return std::nullopt;
    std::vector<uint8_t> color, and_mask, xor_mask;
    bool ok = true;
    if (monochrome) {
        // Top half AND mask, bottom half XOR image.
        std::vector<uint8_t> both;
        ok = read_bits(dc, bm.info.hbmMask, width, height * 2, both);
        if (ok) {
            size_t half = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            and_mask.assign(both.begin(), both.begin() + static_cast<ptrdiff_t>(half));
            xor_mask.assign(both.begin() + static_cast<ptrdiff_t>(half), both.end());
        }
    } else {
        ok = read_bits(dc, bm.info.hbmColor, width, height, color) &&
             read_bits(dc, bm.info.hbmMask, width, height, and_mask);
    }
    ReleaseDC(nullptr, dc);
    if (!ok) return std::nullopt;

    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);

    bool has_alpha = false;
    if (!monochrome) {
        for (size_t i = 0; i < pixels && !has_alpha; ++i) has_alpha = color[i * 4 + 3] != 0;
    }
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t* d = &img.rgba[i * 4];
        const bool transparent = and_mask[i * 4] != 0;  // AND bit set: screen shows through
        if (monochrome) {
            uint8_t v = xor_mask[i * 4] ? 255 : 0;
            d[0] = d[1] = d[2] = v;
            d[3] = transparent ? 0 : 255;
            continue;
        }
        const uint8_t* s = &color[i * 4];
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = has_alpha ? s[3] : (transparent ? 0 : 255);
    }
    return img;
}

}  // namespace brosys::win::tray
