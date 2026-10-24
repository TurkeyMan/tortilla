// Renders the file manager menu's icons (src/shell/menu_icons.cpp) to
// <outdir>/<group>.ico, each drawn at every size a menu uses rather than
// scaled, plus <outdir>/sheet.png: all of them large, to look over.
// Needs Qt 6 (QtGui), so it isn't part of the build:
//   g++ -std=c++23 -fPIC -Isrc -Irapport/core/include tools/make_menu_icons.cpp \
//       src/shell/menu_icons.cpp -o make_menu_icons $(pkg-config --cflags --libs Qt6Gui)
//   QT_QPA_PLATFORM=offscreen ./make_menu_icons data/windows/menu
#include "shell/menu_icons.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace
{

QColor qt(rap::color c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

// The painter calls the icons make, onto a QImage.
class qt_painter : public rap::painter
{
public:
    explicit qt_painter(QPainter& p) : p_(p) { p_.setRenderHint(QPainter::Antialiasing); }

    rap::color resolve(const rap::paint& paint) const override
    {
        if (auto* c = std::get_if<rap::color>(&paint)) return *c;
        return {0, 0, 0};
    }
    void line(rap::point_f a, rap::point_f b, const rap::paint& c, float width,
              rap::line_cap cap) override
    {
        p_.setPen(pen(c, width, cap));
        p_.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    }
    void curve(rap::point_f a, rap::point_f c1, rap::point_f c2, rap::point_f b,
               const rap::paint& c, float width) override
    {
        QPainterPath path(QPointF(a.x, a.y));
        path.cubicTo(QPointF(c1.x, c1.y), QPointF(c2.x, c2.y), QPointF(b.x, b.y));
        p_.setPen(pen(c, width, rap::line_cap::round));
        p_.setBrush(Qt::NoBrush);
        p_.drawPath(path);
    }
    void circle(rap::point_f c, float r, std::optional<rap::paint> fill,
                std::optional<rap::paint> stroke) override
    {
        set(fill, stroke, 1);
        p_.drawEllipse(QPointF(c.x, c.y), r, r);
    }
    void rect(rap::rect_f r, float radius, std::optional<rap::paint> fill,
              std::optional<rap::paint> stroke) override
    {
        set(fill, stroke, 1);
        p_.drawRoundedRect(QRectF(r.x, r.y, r.w, r.h), radius, radius);
    }
    void polygon(const std::vector<rap::point_f>& pts, std::optional<rap::paint> fill,
                 std::optional<rap::paint> stroke, float width) override
    {
        QPolygonF poly;
        for (auto q : pts) poly << QPointF(q.x, q.y);
        set(fill, stroke, width);
        p_.drawPolygon(poly);
    }
    void arc(rap::point_f c, float r, float start, float end, const rap::paint& paint, float width,
             rap::line_cap cap) override
    {
        constexpr float deg = 180 / std::numbers::pi_v<float>;
        p_.setPen(pen(paint, width, cap));
        p_.setBrush(Qt::NoBrush);
        // Qt: counter-clockwise from 3 o'clock, in 1/16 degrees.
        p_.drawArc(QRectF(c.x - r, c.y - r, 2 * r, 2 * r), int(-start * deg * 16),
                   int(-(end - start) * deg * 16));
    }
    void image(const rap::image&, rap::rect_f, rap::rect_f, rap::filter, float) override {}
    float device_scale() const override { return 1; }
    void push_clip(rap::rect_f) override {}
    void pop_clip() override {}
    void text(rap::point_f at, const std::string& s, const rap::text_style& style) override
    {
        p_.setFont(font(style));
        p_.setPen(qt(resolve(style.fg.value_or(rap::color{}))));
        const QFontMetricsF m(p_.font());
        p_.drawText(QPointF(at.x, at.y + m.ascent()), QString::fromStdString(s));
    }
    rap::size_f measure(const std::string& s, const rap::text_style& style) const override
    {
        const QFontMetricsF m(font(style));
        return {float(m.horizontalAdvance(QString::fromStdString(s))), float(m.height())};
    }

private:
    QPen pen(const rap::paint& c, float width, rap::line_cap cap) const
    {
        QPen pen(qt(resolve(c)), width);
        pen.setCapStyle(cap == rap::line_cap::round ? Qt::RoundCap : Qt::FlatCap);
        pen.setJoinStyle(Qt::RoundJoin);
        return pen;
    }
    void set(const std::optional<rap::paint>& fill, const std::optional<rap::paint>& stroke,
             float width)
    {
        p_.setBrush(fill ? QBrush(qt(resolve(*fill))) : QBrush(Qt::NoBrush));
        p_.setPen(stroke ? pen(*stroke, width, rap::line_cap::round) : QPen(Qt::NoPen));
    }
    static QFont font(const rap::text_style& style)
    {
        QFont f("Segoe UI");
        f.setPixelSize(std::max(1, int(std::lround(style.size / 0.75f))));
        f.setBold(style.bold);
        return f;
    }

    QPainter& p_;
};

QImage render(std::string_view group, int size, int scale = 1)
{
    QImage out(size * scale, size * scale, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.scale(scale, scale);
    qt_painter painter(p);
    tortilla::paint_menu_icon(painter, group, 0, 0, float(size));
    return out;
}

QByteArray ico(std::string_view group)
{
    std::vector<std::pair<int, QByteArray>> images;
    for (int n : {16, 20, 24, 32, 40, 48})
    {
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        render(group, n).convertToFormat(QImage::Format_ARGB32).save(&buf, "PNG");
        images.push_back({n, png});
    }
    QByteArray out;
    auto u16 = [&](int v) { out.append(char(v & 0xff)).append(char((v >> 8) & 0xff)); };
    auto u32 = [&](qint64 v)
    {
        u16(int(v & 0xffff));
        u16(int(v >> 16));
    };
    u16(0);
    u16(1);  // icon
    u16(int(images.size()));
    qint64 offset = 6 + 16 * qint64(images.size());
    for (auto& [n, png] : images)
    {
        out.append(char(n)).append(char(n));
        out.append(char(0)).append(char(0));  // no palette, reserved
        u16(1);                               // planes
        u16(32);                              // bits per pixel
        u32(png.size());
        u32(offset);
        offset += png.size();
    }
    for (auto& [n, png] : images) out.append(png);
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: make_menu_icons outdir\n");
        return 2;
    }
    const QString dir = argv[1];
    QDir().mkpath(dir);
    const auto groups = tortilla::menu_icon_groups();
    for (std::string_view g : groups)
    {
        QFile f(dir + "/" + QString::fromUtf8(g.data(), qsizetype(g.size())) + ".ico");
        if (!f.open(QIODevice::WriteOnly)) return 1;
        f.write(ico(g));
    }

    // The sheet: each icon at 16 px, those pixels magnified, and drawn at 64.
    const int cols = 6, cell = 160;
    const int rows = int(groups.size() + cols - 1) / cols;
    QImage sheet(cols * cell, rows * cell, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor(0xf6, 0xf5, 0xf4));
    QPainter p(&sheet);
    for (size_t i = 0; i < groups.size(); ++i)
    {
        const int x = int(i % cols) * cell, y = int(i / cols) * cell;
        p.drawImage(x + 4, y + 4, render(groups[i], 16));
        p.drawImage(x + 24, y + 4, render(groups[i], 16).scaled(64, 64));
        p.drawImage(x + 92, y + 4, render(groups[i], 16, 4));
        p.setPen(Qt::black);
        p.setFont(QFont("Sans", 7));
        p.drawText(QRect(x, y + 72, cell, 20), Qt::AlignCenter,
                   QString::fromUtf8(groups[i].data(), qsizetype(groups[i].size())));
    }
    p.end();
    sheet.save(dir + "/sheet.png");
    std::printf("%zu icons\n", groups.size());
    return 0;
}
