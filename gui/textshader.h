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

#ifndef TEXTSHADER_H
#define TEXTSHADER_H

#include <QColor>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <memory>
#include <string>
#include <vector>

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// TextShaderData is built text, ready for the TextShader: a textured quad per character, in world
// coordinates.
struct TextShaderData
{
    // x, y, u, v per vertex, four vertices per character
    std::vector<GLfloat> vertices;
    std::vector<GLuint> indices;

    int last_render = 0;

    void clear(void)
    {
        vertices.clear();
        indices.clear();
    }
};

// TextShader draws text (TYPE_LABEL graphic elements) with a monospaced font: the printable ASCII
// characters are drawn once into a texture, and a string becomes a strip of quads that each show
// one character of that texture. Text is sized in world coordinates, so it zooms with the view.
class TextShader
{
  public:
    TextShader(QObject *parent) : parent_(parent) {}

    // Set up the font and the glyph texture layout. Must be called once, on the GUI thread, before
    // build() is used (on any thread).
    static void init_font(void);

    // Add `text`, centred in the box (x1, y1) - (x2, y2) and as large as fits.
    static void build(TextShaderData &out, const std::string &text, float x1, float y1, float x2, float y2);

    // Must be called on initialization (with the OpenGL context current), with the number of
    // buffer slots.
    bool compile(int slots);

    void update_vbos(int slot, const TextShaderData &data);

    // Draw the text of a slot in a colour with a given M/V/P transformation.
    void draw(int slot, const QColor &color, const QMatrix4x4 &projection);

  private:
    struct Buffers
    {
        QOpenGLBuffer vertices{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer index{QOpenGLBuffer::IndexBuffer};
        QOpenGLVertexArrayObject vao;
        int indices = 0;
        int last_vbo_update = 0;
    };

    QObject *parent_;
    QOpenGLShaderProgram *program_ = nullptr;
    GLuint texture_ = 0;
    std::vector<std::unique_ptr<Buffers>> buffers_;
    struct
    {
        GLuint position, texcoord;
    } attributes_;
    struct
    {
        GLuint projection, color, glyphs;
    } uniforms_;

    static constexpr const char *vertexShaderSource_ = "#version 150\n"
                                                       "in highp vec2 position;\n"
                                                       "in highp vec2 texcoord;\n"
                                                       "uniform highp mat4 projection;\n"
                                                       "out highp vec2 uv;\n"
                                                       "void main() {\n"
                                                       "   uv = texcoord;\n"
                                                       "   gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                                       "}\n";

    static constexpr const char *fragmentShaderSource_ =
            "#version 150\n"
            "uniform sampler2D glyphs;\n"
            "uniform lowp vec4 color;\n"
            "in highp vec2 uv;\n"
            "out vec4 Out_Color;\n"
            "void main() {\n"
            "   Out_Color = vec4(color.rgb, color.a * texture(glyphs, uv).a);\n"
            "}\n";
};

NEXTPNR_NAMESPACE_END

#endif // TEXTSHADER_H
