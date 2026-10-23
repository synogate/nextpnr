/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The nextpnr Authors.
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "textshader.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QOpenGLContext>
#include <QPainter>

#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// The glyph texture: the characters ' ' to '~' in a grid of equal cells
struct GlyphAtlas
{
    static constexpr int first = 32, last = 126, cols = 16, rows = 6, pad = 2;
    QImage image;
    int cell_w = 0, cell_h = 0; // cell size in pixels, without the padding
    float aspect = 0.6f;        // cell width / height
    bool ready = false;

    // Texture coordinates of the cell of character c: left, top, right, bottom
    void uv(unsigned char c, float &u0, float &v0, float &u1, float &v1) const
    {
        if (c < first || c > last)
            c = '?';
        int i = c - first, col = i % cols, row = i / cols;
        float w = image.width(), h = image.height();
        u0 = (col * (cell_w + 2 * pad) + pad) / w;
        v0 = (row * (cell_h + 2 * pad) + pad) / h;
        u1 = u0 + cell_w / w;
        v1 = v0 + cell_h / h;
    }
};

GlyphAtlas atlas;

} // namespace

void TextShader::init_font(void)
{
    if (atlas.ready)
        return;
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(48);
    QFontMetrics metrics(font);
    atlas.cell_w = metrics.horizontalAdvance(QLatin1Char('M'));
    atlas.cell_h = metrics.height();
    atlas.aspect = float(atlas.cell_w) / atlas.cell_h;
    int w = GlyphAtlas::cols * (atlas.cell_w + 2 * GlyphAtlas::pad);
    int h = GlyphAtlas::rows * (atlas.cell_h + 2 * GlyphAtlas::pad);
    atlas.image = QImage(w, h, QImage::Format_RGBA8888);
    atlas.image.fill(Qt::transparent);
    QPainter painter(&atlas.image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(Qt::white);
    for (int c = GlyphAtlas::first; c <= GlyphAtlas::last; c++) {
        int i = c - GlyphAtlas::first, col = i % GlyphAtlas::cols, row = i / GlyphAtlas::cols;
        int x = col * (atlas.cell_w + 2 * GlyphAtlas::pad) + GlyphAtlas::pad;
        int y = row * (atlas.cell_h + 2 * GlyphAtlas::pad) + GlyphAtlas::pad;
        painter.drawText(QPointF(x, y + metrics.ascent()), QString(QLatin1Char(char(c))));
    }
    painter.end();
    atlas.ready = true;
}

void TextShader::build(TextShaderData &out, const std::string &text, float x1, float y1, float x2, float y2)
{
    float n = text.size(), box_w = x2 - x1, box_h = y2 - y1;
    if (n == 0 || box_w <= 0 || box_h <= 0)
        return;
    // Character height: the box height, or less for the text to fit the box width
    float h = std::min(box_h, box_w / (n * atlas.aspect)), w = h * atlas.aspect;
    float x = x1 + (box_w - n * w) / 2, y = y1 + (box_h - h) / 2;
    for (size_t i = 0; i < text.size(); i++) {
        float u0, v0, u1, v1;
        atlas.uv(text[i], u0, v0, u1, v1);
        float cx0 = x + i * w, cx1 = cx0 + w;
        GLuint base = out.vertices.size() / 4;
        // Image rows go down, world y goes up: the top of the cell (v0) is at y + h
        for (GLfloat v : {cx0, y, u0, v1, cx1, y, u1, v1, cx1, y + h, u1, v0, cx0, y + h, u0, v0})
            out.vertices.push_back(v);
        for (GLuint k : {0u, 1u, 2u, 0u, 2u, 3u})
            out.indices.push_back(base + k);
    }
}

bool TextShader::compile(int slots)
{
    init_font();
    program_ = new QOpenGLShaderProgram(parent_);
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource_);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource_);
    if (!program_->link()) {
        printf("could not link program: %s\n", program_->log().toStdString().c_str());
        return false;
    }
    program_->bind();
    attributes_.position = program_->attributeLocation("position");
    attributes_.texcoord = program_->attributeLocation("texcoord");
    uniforms_.projection = program_->uniformLocation("projection");
    uniforms_.color = program_->uniformLocation("color");
    uniforms_.glyphs = program_->uniformLocation("glyphs");
    program_->release();

    // The glyph texture, with mipmaps for small text
    auto gl = QOpenGLContext::currentContext()->functions();
    gl->glGenTextures(1, &texture_);
    gl->glBindTexture(GL_TEXTURE_2D, texture_);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas.image.width(), atlas.image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     atlas.image.constBits());
    gl->glGenerateMipmap(GL_TEXTURE_2D);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glBindTexture(GL_TEXTURE_2D, 0);

    buffers_.clear();
    for (int slot = 0; slot < slots; slot++) {
        buffers_.push_back(std::make_unique<Buffers>());
        Buffers &b = *buffers_.back();
        if (!b.vao.create() || !b.vertices.create() || !b.index.create())
            log_abort();
        b.vertices.setUsagePattern(QOpenGLBuffer::StaticDraw);
        b.index.setUsagePattern(QOpenGLBuffer::StaticDraw);
    }
    return true;
}

void TextShader::update_vbos(int slot, const TextShaderData &data)
{
    Buffers &b = *buffers_.at(slot);
    if (b.last_vbo_update == data.last_render)
        return;
    b.last_vbo_update = data.last_render;
    b.indices = data.indices.size();
    if (b.indices == 0)
        return;
    b.vertices.bind();
    b.vertices.allocate(data.vertices.data(), sizeof(GLfloat) * data.vertices.size());
    b.index.bind();
    b.index.allocate(data.indices.data(), sizeof(GLuint) * data.indices.size());
}

void TextShader::draw(int slot, const QColor &color, const QMatrix4x4 &projection)
{
    Buffers &b = *buffers_.at(slot);
    if (b.indices == 0)
        return;
    auto gl = QOpenGLContext::currentContext()->functions();
    program_->bind();
    b.vao.bind();
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, texture_);
    program_->setUniformValue(uniforms_.glyphs, 0);
    program_->setUniformValue(uniforms_.projection, projection);
    program_->setUniformValue(uniforms_.color, color.redF(), color.greenF(), color.blueF(), color.alphaF());

    b.vertices.bind();
    program_->enableAttributeArray(attributes_.position);
    program_->setAttributeBuffer(attributes_.position, GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    program_->enableAttributeArray(attributes_.texcoord);
    program_->setAttributeBuffer(attributes_.texcoord, GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));
    b.index.bind();
    gl->glDrawElements(GL_TRIANGLES, b.indices, GL_UNSIGNED_INT, (void *)0);

    program_->disableAttributeArray(attributes_.position);
    program_->disableAttributeArray(attributes_.texcoord);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    b.vao.release();
    program_->release();
}

NEXTPNR_NAMESPACE_END
