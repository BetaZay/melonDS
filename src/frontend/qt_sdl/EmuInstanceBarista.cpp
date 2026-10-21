// SPDX-License-Identifier: GPL-3.0-or-later
#include "EmuInstance.h"
#include "OpenGLSupport.h"
#include <algorithm>
#include <QImage>
#include <QPainter>

void EmuInstance::baristaConfigure()
{
    if (instanceID != 0) return;
    Barista::Options options;
    options.enabled = globalCfg.GetBool("Barista.Enabled");
    options.input = globalCfg.GetBool("Barista.Input");
    options.screen = static_cast<Barista::ScreenMode>(globalCfg.GetInt("Barista.Screen"));
    options.socketPath = globalCfg.GetString("Barista.SocketPath");
    QImage canvas(Barista::Width, Barista::Height, QImage::Format_RGB888);
    canvas.fill(Qt::black);
    QPainter painter(&canvas);
    QImage logo(":/melon-logo");
    if (!logo.isNull())
    {
        logo = logo.scaled(384, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawImage((canvas.width()-logo.width())/2, (canvas.height()-logo.height())/2, logo);
    }
    painter.end();
    std::vector<uint8_t> rgb(canvas.constBits(), canvas.constBits() + Barista::Width * Barista::Height * 3);
    barista.Configure(options, std::move(rgb));
}

void EmuInstance::baristaCaptureFrame()
{
    if (!barista.WantsFrame() || !nds) return;
    void* top = nullptr;
    void* bottom = nullptr;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        barista.SubmitScreens(static_cast<uint32_t*>(top), static_cast<uint32_t*>(bottom), 256, 192);
        return;
    }
#ifdef OGLRENDERER_ENABLED
    // The hardware renderers return a two-layer texture. Read the composed DS
    // screens before desktop layouts/OSD, downsampling on the GPU at high scales.
    GLint readFB, drawFB, renderbuffer, texture, packBuffer, packAlignment, rowLength, skipRows, skipPixels;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFB);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFB);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &texture);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D_ARRAY, *static_cast<GLuint*>(top));
    GLint width, height;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
    const int captureWidth = std::min(width, 640);
    const int captureHeight = std::min(height, 480);
    GLuint fb[2], color;
    glGenFramebuffers(2, fb);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, captureWidth, captureHeight);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    std::vector<uint32_t> pixels(captureWidth * captureHeight * 2);
    bool complete = true;
    for (int layer = 0; layer < 2; layer++)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, *static_cast<GLuint*>(top), 0, layer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            complete = false;
            break;
        }
        glBlitFramebuffer(0, 0, width, height, 0, 0, captureWidth, captureHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[1]);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, captureWidth, captureHeight, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data() + layer * captureWidth * captureHeight);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFB);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFB);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(2, fb);
    glDeleteRenderbuffers(1, &color);
    if (complete) barista.SubmitScreens(pixels.data(), pixels.data() + captureWidth * captureHeight, captureWidth, captureHeight);
#endif
}
