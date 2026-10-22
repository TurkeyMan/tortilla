// Makes Tortilla's icon set from the master PNG: the drawing cropped to its
// opaque pixels, centred on a square, and area-averaged down to each size.
// Writes <outdir>/hicolor/NxN/apps/tortilla.png and <outdir>/tortilla.ico.
// Needs Qt 6 (QtGui), so it isn't part of the build:
//   g++ -std=c++20 -fPIC tools/make_icons.cpp -o make_icons $(pkg-config --cflags --libs Qt6Gui)
//   ./make_icons data/icons/tortilla-master.png out
// then copy out/hicolor to data/icons/ and out/tortilla.ico to data/windows/.
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <cstdio>
#include <vector>

static QImage square(const QImage& src)
{
    QRect box;
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x)
            if (qAlpha(src.pixel(x, y)) > 8) box |= QRect(x, y, 1, 1);
    std::printf("opaque box %d,%d %dx%d of %dx%d\n", box.x(), box.y(), box.width(), box.height(), src.width(), src.height());
    const int side = std::max(box.width(), box.height());
    QImage out(side, side, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawImage(QPoint((side - box.width()) / 2, (side - box.height()) / 2), src, box);
    return out;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: make_icons master.png outdir\n");
        return 2;
    }
    const QImage master = square(QImage(argv[1]).convertToFormat(QImage::Format_ARGB32_Premultiplied));
    const QString out = argv[2];
    auto at = [&](int n) { return master.scaled(n, n, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32); };

    // Linux: the hicolor theme's sizes.
    for (int n : {16, 22, 24, 32, 48, 64, 96, 128, 256, 512})
    {
        const QString dir = out + QString("/hicolor/%1x%1/apps").arg(n);
        QDir().mkpath(dir);
        at(n).save(dir + "/tortilla.png");
    }

    // Windows: one .ico of PNG images, every size the shell asks for.
    std::vector<std::pair<int, QByteArray>> images;
    for (int n : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256})
    {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        at(n).save(&buf, "PNG");
        images.push_back({n, png});
    }
    QByteArray ico;
    auto u16 = [&](int v) { ico.append(char(v & 0xff)).append(char((v >> 8) & 0xff)); };
    auto u32 = [&](qint64 v) { u16(int(v & 0xffff)); u16(int(v >> 16)); };
    u16(0);
    u16(1);  // icon
    u16(int(images.size()));
    qint64 offset = 6 + 16 * qint64(images.size());
    for (auto& [n, png] : images)
    {
        ico.append(char(n >= 256 ? 0 : n)).append(char(n >= 256 ? 0 : n));
        ico.append(char(0)).append(char(0));  // no palette, reserved
        u16(1);   // planes
        u16(32);  // bits per pixel
        u32(png.size());
        u32(offset);
        offset += png.size();
    }
    for (auto& [n, png] : images) ico.append(png);
    QFile f(out + "/tortilla.ico");
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(ico);
    std::printf("tortilla.ico: %lld bytes\n", qint64(ico.size()));
    return 0;
}
