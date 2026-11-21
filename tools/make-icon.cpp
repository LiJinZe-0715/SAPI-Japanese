// make-icon — draws src/res/app.ico, the mark every binary in this project carries.
//
// Generated instead of hand-drawn for the same reason the catalog table is generated: the same
// mark has to exist at nine sizes, anti-aliased, and a paint program cannot be diffed, reviewed
// or re-run when a colour changes. Run this by hand after editing the geometry below, then commit
// the .ico — the build never runs it, because rc.exe needs the file before anything compiles.
//
//   voicevox-make-icon.exe [out.ico]        (default: src/res/app.ico)
//
// The shape is a speech bubble with three bars in it on a rounded square: "voice" without a
// glyph, so it stays readable at 16x16 and needs no font (which would make the output depend on
// whatever is installed on the build machine).

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Rgb {
    float r, g, b;
};

// Gradient of the tile, top-left to bottom-right.
constexpr Rgb kTop = {1.00f, 0.42f, 0.64f};     // #FF6BA3
constexpr Rgb kBottom = {0.42f, 0.36f, 0.91f};  // #6C5CE7
constexpr Rgb kInk = {1.00f, 1.00f, 1.00f};

// 4x4 samples per output pixel. Coverage is accumulated in premultiplied form, so the silhouette
// edge and the bubble/bars edges both come out smooth; 16 levels are enough at icon sizes.
constexpr int kSupersample = 4;

// Everything below 24 px drops the bars: at 16 px a bar is 1.4 px wide and three of them with
// gaps turn into grey mush. The bubble alone still reads as "speech" at that size.
constexpr int kBarsMinSize = 24;

constexpr int kSizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};

// Signed distance to a rounded rectangle, negative inside. Working in normalised [0,1] space keeps
// one geometry for every size, which is the whole point of generating the icon.
float RoundRectDistance(float u, float v, float x0, float y0, float x1, float y1, float radius) {
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f - radius;
    const float hy = (y1 - y0) * 0.5f - radius;
    const float qx = std::fabs(u - cx) - hx;
    const float qy = std::fabs(v - cy) - hy;
    const float ox = qx > 0 ? qx : 0.0f;
    const float oy = qy > 0 ? qy : 0.0f;
    const float outside = std::sqrt(ox * ox + oy * oy);
    const float inside = (qx > qy ? qx : qy);
    return outside + (inside < 0 ? inside : 0.0f) - radius;
}

bool InTriangle(float u, float v, float ax, float ay, float bx, float by, float cx, float cy) {
    const float d1 = (u - bx) * (ay - by) - (ax - bx) * (v - by);
    const float d2 = (u - cx) * (by - cy) - (bx - cx) * (v - cy);
    const float d3 = (u - ax) * (cy - ay) - (cx - ax) * (v - ay);
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    const bool positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

// The tile. The 0.015 inset leaves a hairline of transparency so the icon does not look pasted
// onto the taskbar.
bool InTile(float u, float v) {
    return RoundRectDistance(u, v, 0.015f, 0.015f, 0.985f, 0.985f, 0.225f) <= 0.0f;
}

// Body plus tail. The tail's base sits well inside the body so the union reads as one outline
// instead of a box with a triangle stuck to it.
bool InBubble(float u, float v) {
    if (RoundRectDistance(u, v, 0.125f, 0.185f, 0.875f, 0.705f, 0.175f) <= 0.0f) return true;
    return InTriangle(u, v, 0.255f, 0.560f, 0.470f, 0.560f, 0.215f, 0.860f);
}

// Three bars, centred in the bubble body, with the heights of a level meter. Wide enough that a
// bar is still 3 px at 32x32 — anything thinner turns into a grey line there.
bool InBars(float u, float v) {
    constexpr float kWidth = 0.098f;
    constexpr float kPitch = 0.152f;
    constexpr float kLeft = 0.294f;
    constexpr float kMid = 0.445f;  // vertical centre of the bubble body
    const float heights[3] = {0.150f, 0.285f, 0.210f};
    for (int i = 0; i < 3; ++i) {
        const float x0 = kLeft + i * kPitch;
        const float y0 = kMid - heights[i] * 0.5f;
        const float y1 = kMid + heights[i] * 0.5f;
        if (RoundRectDistance(u, v, x0, y0, x0 + kWidth, y1, kWidth * 0.5f) <= 0.0f) return true;
    }
    return false;
}

Rgb Gradient(float u, float v) {
    float t = v * 0.88f + u * 0.12f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return {kTop.r + (kBottom.r - kTop.r) * t, kTop.g + (kBottom.g - kTop.g) * t,
            kTop.b + (kBottom.b - kTop.b) * t};
}

// One sample: the tile is opaque, everything else is cut out of it.
Rgb Sample(float u, float v, bool bars, float* coverage) {
    if (!InTile(u, v)) {
        *coverage = 0.0f;
        return {0.0f, 0.0f, 0.0f};
    }
    *coverage = 1.0f;
    if (!InBubble(u, v)) return Gradient(u, v);
    if (bars && InBars(u, v)) return Gradient(u, v);
    return kInk;
}

struct Pixel {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;  // premultiplied
};

std::vector<Pixel> Render(int size) {
    const int hi = size * kSupersample;
    const bool bars = size >= kBarsMinSize;
    const float samples = (float)(kSupersample * kSupersample);

    std::vector<Pixel> pixels((size_t)size * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Pixel sum;
            for (int sy = 0; sy < kSupersample; ++sy) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const float u = (x * kSupersample + sx + 0.5f) / hi;
                    const float v = (y * kSupersample + sy + 0.5f) / hi;
                    float coverage = 0.0f;
                    const Rgb c = Sample(u, v, bars, &coverage);
                    sum.r += c.r * coverage;
                    sum.g += c.g * coverage;
                    sum.b += c.b * coverage;
                    sum.a += coverage;
                }
            }
            pixels[(size_t)y * size + x] = {sum.r / samples, sum.g / samples, sum.b / samples,
                                            sum.a / samples};
        }
    }
    return pixels;
}

void Append16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back((unsigned char)(value & 0xFF));
    out.push_back((unsigned char)((value >> 8) & 0xFF));
}

void Append32(std::vector<unsigned char>& out, unsigned value) {
    for (int i = 0; i < 4; ++i) out.push_back((unsigned char)((value >> (8 * i)) & 0xFF));
}

unsigned char ToByte(float value) {
    const float scaled = value * 255.0f + 0.5f;
    if (scaled <= 0.0f) return 0;
    if (scaled >= 255.0f) return 255;
    return (unsigned char)scaled;
}

// One icon image: a BITMAPINFOHEADER, bottom-up 32bpp BGRA pixels, then the 1bpp AND mask.
// The mask is all zeros — Vista and later take transparency from the alpha channel, but readers
// that predate it would treat a missing mask as "everything opaque".
std::vector<unsigned char> EncodeImage(int size, const std::vector<Pixel>& pixels) {
    std::vector<unsigned char> image;
    image.reserve(40 + (size_t)size * size * 4 + (size_t)size * 4);

    Append32(image, 40);                 // biSize
    Append32(image, (unsigned)size);     // biWidth
    Append32(image, (unsigned)size * 2); // biHeight: doubled, the mask rows follow the pixels
    Append16(image, 1);                  // biPlanes
    Append16(image, 32);                 // biBitCount
    Append32(image, 0);                  // BI_RGB
    Append32(image, (unsigned)(size * size * 4));
    Append32(image, 0);  // biXPelsPerMeter
    Append32(image, 0);  // biYPelsPerMeter
    Append32(image, 0);  // biClrUsed
    Append32(image, 0);  // biClrImportant

    for (int y = size - 1; y >= 0; --y) {
        for (int x = 0; x < size; ++x) {
            const Pixel& p = pixels[(size_t)y * size + x];
            const float alpha = p.a <= 0.0f ? 0.0f : p.a;
            const float scale = alpha > 0.0f ? 1.0f / alpha : 0.0f;
            image.push_back(ToByte(p.b * scale));
            image.push_back(ToByte(p.g * scale));
            image.push_back(ToByte(p.r * scale));
            image.push_back(ToByte(alpha));
        }
    }

    const int maskRowBytes = ((size + 31) / 32) * 4;
    image.insert(image.end(), (size_t)maskRowBytes * size, 0);
    return image;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* path = argc > 1 ? argv[1] : L"src/res/app.ico";

    const int count = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
    std::vector<std::vector<unsigned char>> images;
    images.reserve(count);
    for (int i = 0; i < count; ++i) images.push_back(EncodeImage(kSizes[i], Render(kSizes[i])));

    std::vector<unsigned char> file;
    Append16(file, 0);          // reserved
    Append16(file, 1);          // type: icon
    Append16(file, (unsigned)count);

    unsigned offset = 6 + 16 * (unsigned)count;
    for (int i = 0; i < count; ++i) {
        const int size = kSizes[i];
        // 256 is encoded as 0: the field is one byte and cannot hold 256.
        file.push_back((unsigned char)(size >= 256 ? 0 : size));
        file.push_back((unsigned char)(size >= 256 ? 0 : size));
        file.push_back(0);  // colour count: true colour
        file.push_back(0);  // reserved
        Append16(file, 1);  // planes
        Append16(file, 32); // bits per pixel
        Append32(file, (unsigned)images[i].size());
        Append32(file, offset);
        offset += (unsigned)images[i].size();
    }
    for (const std::vector<unsigned char>& image : images) file.insert(file.end(), image.begin(), image.end());

    FILE* out = nullptr;
    if (_wfopen_s(&out, path, L"wb") != 0 || out == nullptr) {
        std::fwprintf(stderr, L"cannot write %s\n", path);
        return 1;
    }
    const size_t written = std::fwrite(file.data(), 1, file.size(), out);
    std::fclose(out);
    if (written != file.size()) {
        std::fwprintf(stderr, L"short write to %s\n", path);
        return 1;
    }

    std::wprintf(L"%s: %d sizes, %zu bytes\n", path, count, file.size());
    return 0;
}
