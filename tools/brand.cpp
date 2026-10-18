// Build-time asset generator; uses only Windows GDI+ and COM.
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <filesystem>
#include <fstream>
#include <gdiplus.h>
#include <objidl.h>
#include <stdexcept>
#include <vector>
#include <windows.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
using namespace Gdiplus;
namespace fs = std::filesystem;
CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
CLSID bmp = {0x557cf400, 0x1a04, 0x11d3, {0x9a, 0x73, 0, 0, 0xf8, 0x1e, 0xf3, 0x2e}};
void draw(Graphics &g, float x, float y, float size) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 256.0f, size / 256.0f);
    SolidBrush navy(Color(255, 24, 42, 68)), drawer(Color(255, 45, 65, 94)), mint(Color(255, 94, 234, 204)),
        white(Color(255, 245, 250, 255));
    GraphicsPath body;
    body.AddArc(12, 12, 48, 48, 180, 90);
    body.AddArc(196, 12, 48, 48, 270, 90);
    body.AddArc(196, 196, 48, 48, 0, 90);
    body.AddArc(12, 196, 48, 48, 90, 90);
    body.CloseFigure();
    g.FillPath(&navy, &body);
    g.FillRectangle(&drawer, 38, 48, 178, 62);
    g.FillRectangle(&drawer, 38, 126, 178, 62);
    g.FillRectangle(&mint, 104, 68, 46, 9);
    g.FillRectangle(&mint, 72, 146, 40, 9);
    PointF arrow[] = {{139, 103}, {139, 227}, {166, 204}, {184, 239}, {204, 228}, {187, 195}, {222, 190}};
    g.FillPolygon(&white, arrow, 7);
    Pen outline(Color(255, 24, 42, 68), 6);
    g.DrawPolygon(&outline, arrow, 7);
    g.ResetTransform();
}
void put16(std::vector<BYTE> &b, unsigned n) {
    b.push_back(static_cast<BYTE>(n));
    b.push_back(static_cast<BYTE>(n >> 8));
}
void put32(std::vector<BYTE> &b, unsigned n) {
    put16(b, n & 65535);
    put16(b, n >> 16);
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2)
        return 2;
    ULONG_PTR token = 0;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok)
        return 3;
    int result = 0;
    try {
        fs::path out = argv[1];
        fs::create_directories(out);
        std::vector<std::vector<BYTE>> images;
        std::vector<unsigned> sizes = {16, 32, 48, 64, 128, 256};
        for (auto size : sizes) {
            Bitmap icon(size, size, PixelFormat32bppARGB);
            Graphics g(&icon);
            g.Clear(Color(0, 0, 0, 0));
            draw(g, 0, 0, static_cast<float>(size));
            IStream *stream = nullptr;
            if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
                throw std::runtime_error("image stream");
            if (icon.Save(stream, &png, nullptr) != Ok) {
                stream->Release();
                throw std::runtime_error("PNG encoding");
            }
            STATSTG stat{};
            stream->Stat(&stat, STATFLAG_NONAME);
            LARGE_INTEGER zero{};
            stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            std::vector<BYTE> bytes(stat.cbSize.LowPart);
            ULONG got = 0;
            stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &got);
            stream->Release();
            if (got != bytes.size())
                throw std::runtime_error("PNG read");
            images.push_back(std::move(bytes));
        }
        std::vector<BYTE> file;
        put16(file, 0);
        put16(file, 1);
        put16(file, static_cast<unsigned>(images.size()));
        unsigned offset = 6 + 16 * static_cast<unsigned>(images.size());
        for (size_t i = 0; i < images.size(); ++i) {
            file.push_back(static_cast<BYTE>(sizes[i] == 256 ? 0 : sizes[i]));
            file.push_back(file.back());
            file.push_back(0);
            file.push_back(0);
            put16(file, 1);
            put16(file, 32);
            put32(file, static_cast<unsigned>(images[i].size()));
            put32(file, offset);
            offset += static_cast<unsigned>(images[i].size());
        }
        for (auto &image : images)
            file.insert(file.end(), image.begin(), image.end());
        std::ofstream icon(out / L"CursorCabinet.ico", std::ios::binary);
        icon.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
        icon.close();
        {
            Bitmap image(164, 314, PixelFormat24bppRGB);
            Graphics g(&image);
            g.Clear(Color(255, 24, 42, 68));
            draw(g, 15, 68, 134);
            SolidBrush ink(Color(255, 240, 247, 255));
            FontFamily family(L"Segoe UI");
            Font font(&family, 12, FontStyleBold, UnitPixel);
            g.DrawString(L"CursorCabinet", -1, &font, PointF(37, 220), &ink);
            Font caption(&family, 10, FontStyleRegular, UnitPixel);
            g.DrawString(L"A new point of view.", -1, &caption, PointF(32, 243), &ink);
            if (image.Save((out / L"wizard.bmp").c_str(), &bmp, nullptr) != Ok)
                throw std::runtime_error("wizard image");
        }
        {
            Bitmap image(55, 55, PixelFormat24bppRGB);
            Graphics g(&image);
            g.Clear(Color(255, 255, 255, 255));
            draw(g, 1, 1, 53);
            if (image.Save((out / L"wizard-small.bmp").c_str(), &bmp, nullptr) != Ok)
                throw std::runtime_error("wizard icon");
        }
    } catch (const std::exception &) {
        result = 1;
    }
    GdiplusShutdown(token);
    return result;
}
