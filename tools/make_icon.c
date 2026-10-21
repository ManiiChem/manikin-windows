// packs his icon (src/icon.c) into a .ico for the exe, every size stored as a png inside
// (windows is fine with that since vista). png_dir gets each size as its own png too
//   make_icon.exe out.ico [png_dir]

#include <stdio.h>

#include "raylib.h"
#include "icon.h"

static void put16(FILE *f, unsigned v)
{
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}

static void put32(FILE *f, unsigned v)
{
    put16(f, v & 0xffff);
    put16(f, v >> 16);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: make_icon out.ico [png_dir]\n");
        return 1;
    }
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(64, 64, "icon");

    Image img[ICON_COUNT];
    unsigned char *png[ICON_COUNT];
    int len[ICON_COUNT];
    for (int i = 0; i < ICON_COUNT; i++) {
        img[i] = icon_image(ICON_SIZE[i]);
        png[i] = ExportImageToMemory(img[i], ".png", &len[i]);
        if (argc > 2) ExportImage(img[i], TextFormat("%s/icon_%d.png", argv[2], ICON_SIZE[i]));
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "can't write %s\n", argv[1]);
        return 1;
    }
    put16(f, 0);                            // ICONDIR header, reserved, type 1 = icon, count
    put16(f, 1);
    put16(f, ICON_COUNT);
    unsigned offset = 6 + 16 * ICON_COUNT;
    for (int i = 0; i < ICON_COUNT; i++) {  // one ICONDIRENTRY per size
        int s = ICON_SIZE[i];
        fputc(s >= 256 ? 0 : s, f);         // width and height, 0 means 256
        fputc(s >= 256 ? 0 : s, f);
        fputc(0, f);                        // no palette, then a reserved byte
        fputc(0, f);
        put16(f, 1);                        // planes
        put16(f, 32);                       // bits per pixel
        put32(f, (unsigned)len[i]);
        put32(f, offset);
        offset += (unsigned)len[i];
    }
    for (int i = 0; i < ICON_COUNT; i++) fwrite(png[i], 1, (size_t)len[i], f);
    fclose(f);

    for (int i = 0; i < ICON_COUNT; i++) {
        MemFree(png[i]);
        UnloadImage(img[i]);
    }
    CloseWindow();
    printf("wrote %s (%d sizes)\n", argv[1], ICON_COUNT);
    return 0;
}
