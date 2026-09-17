#include <windows.h>
#include <iostream>
#include <vector>
#include <cstdio>

int main() {


    //dimensions
    int width  = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);

    HDC screenDC = GetDC(nullptr);

    //create compatible device context and bitmap
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height);

    HGDIOBJ oldBitmap = SelectObject(memDC, bitmap);



    //copy screen into bitmap
    BitBlt(
        memDC,
        0, 0, width, height,
        screenDC,
        0, 0,
        SRCCOPY
    );

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height; // top-down bitmap
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    BITMAPFILEHEADER bf{};
    bf.bfType = 0x4D42; // "BM"
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bf.bfSize = bf.bfOffBits + width * height * 4;

    std::vector<BYTE> pixels(width * height * 4);

    // copy screen onto bitmap
    GetDIBits(
        memDC,
        bitmap,
        0,
        height,
        pixels.data(),
        reinterpret_cast<BITMAPINFO*>(&bi),
        DIB_RGB_COLORS
    );


    // Save file
    FILE* file = nullptr;
    fopen_s(&file, "screenshot.bmp", "wb");

    if (file) {
        fwrite(&bf, sizeof(bf), 1, file);
        fwrite(&bi, sizeof(bi), 1, file);
        fwrite(pixels.data(), pixels.size(), 1, file);
        fclose(file);

        std::cout << "Saved as screenshot.bmp\n";
    }

    // Cleanup
    SelectObject(memDC, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    return 0;
}
