// Makes Tortilla's icon set from the master PNG: the drawing cropped to its
// opaque pixels, centred on a square, and area-averaged down to each size.
// Writes <outdir>/hicolor/NxN/apps/tortilla.png and <outdir>/tortilla.ico.
// Given the shell layer (tools/make_shell_layer.py) too, also each size
// whole, <outdir>/layers/icon-N.png, and split in two, body-N.png and
// shell-N.png: the icon without its shell, and the shell's grey shading. Scaled premultiplied, the two add
// up to the icon at any size, so Tortilla can tint the shell per repository
// with no fringe of the old colour (repo_icon.cpp).
// Needs Qt 6 (QtGui), so it isn't part of the build:
//   g++ -std=c++20 -fPIC tools/make_icons.cpp -o make_icons $(pkg-config --cflags --libs Qt6Gui)
//   ./make_icons data/icons/tortilla-master.png out [data/icons/tortilla-shell-master.png]
// then copy out/hicolor and out/layers to data/icons/ and out/tortilla.ico
// to data/windows/.
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cstdio>
#include <vector>

static QRect opaque_box(const QImage& src)
{
    QRect box;
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x)
            if (qAlpha(src.pixel(x, y)) > 8) box |= QRect(x, y, 1, 1);
    std::printf("opaque box %d,%d %dx%d of %dx%d\n", box.x(), box.y(), box.width(), box.height(), src.width(), src.height());
    return box;
}

// `box` of src, centred on a square.
static QImage square(const QImage& src, const QRect& box)
{
    const int side = std::max(box.width(), box.height());
    QImage out(side, side, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.drawImage(QPoint((side - box.width()) / 2, (side - box.height()) / 2), src, box);
    return out;
}

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4)
    {
        std::fprintf(stderr, "usage: make_icons master.png outdir [shell-master.png]\n");
        return 2;
    }
    const QImage full = QImage(argv[1]).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRect box = opaque_box(full);
    const QImage master = square(full, box);
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
    if (argc < 4) return 0;

    // The icon without its shell, and the shell in grey, the mask (the
    // layer's alpha) parting them. Premultiplied, so scaling keeps them
    // adding up to the icon.
    const QImage layer = QImage(argv[3]).convertToFormat(QImage::Format_ARGB32);
    if (layer.size() != full.size())
    {
        std::fprintf(stderr, "the shell layer isn't the master's size\n");
        return 1;
    }
    const QImage straight = full.convertToFormat(QImage::Format_ARGB32);
    QImage body(full.size(), QImage::Format_ARGB32), shell(full.size(), QImage::Format_ARGB32);
    for (int y = 0; y < full.height(); ++y)
        for (int x = 0; x < full.width(); ++x)
        {
            const QRgb p = straight.pixel(x, y), l = layer.pixel(x, y);
            const bool in_shell = qAlpha(l) >= 128;
            body.setPixel(x, y, in_shell ? 0 : p);
            shell.setPixel(x, y, in_shell ? qRgba(qRed(l), qRed(l), qRed(l), qAlpha(p)) : 0);
        }
    const QImage body_sq = square(body.convertToFormat(QImage::Format_ARGB32_Premultiplied), box);
    const QImage shell_sq = square(shell.convertToFormat(QImage::Format_ARGB32_Premultiplied), box);
    QDir().mkpath(out + "/layers");
    for (int n : {16, 20, 24, 32, 40, 48, 64, 256})
    {
        const QImage icon = at(n);
        const QImage b = body_sq.scaled(n, n, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                             .convertToFormat(QImage::Format_ARGB32);
        QImage s = shell_sq.scaled(n, n, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_ARGB32);
        // The shell's coverage is what the body leaves of the icon's, so the
        // two add up to the icon's alpha exactly, each rounded to 8 bits or
        // not: no pixel on the shell's margin is left short of opaque.
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const QRgb p = s.pixel(x, y);
                const int a = std::max(0, qAlpha(icon.pixel(x, y)) - qAlpha(b.pixel(x, y)));
                s.setPixel(x, y, qRgba(qRed(p), qGreen(p), qBlue(p), a));
            }
        icon.save(out + QString("/layers/icon-%1.png").arg(n));
        b.save(out + QString("/layers/body-%1.png").arg(n));
        s.save(out + QString("/layers/shell-%1.png").arg(n));
    }
    std::printf("layers: icon, body and shell at 8 sizes\n");
    return 0;
}
