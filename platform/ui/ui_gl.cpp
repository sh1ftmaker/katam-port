/* Dear ImGui's draw lists, drawn with the renderer's GL table (gl_api.h).
 *
 * The stock imgui_impl_opengl3 brings its own GL loader, which on Linux
 * dlopens libGL -- the wrong library under an EGL/GLES context (SDL's
 * offscreen driver, Mesa GLES) -- so this is the same ~150 lines against the
 * entry points GlrInit already resolved: GLES 3.0, WebGL2 and GL 3.3 core.
 */

#include "imgui.h"

extern "C" {
#include "render/gl_api.h"
}
#include "ui/ui_internal.h"

#define G(name) GLR_FN(name)

static GLuint sProg, sVao, sVbo, sIbo, sFontTex;
static GLint sLocProj, sLocTex;
static int sEs;

static GLuint CompileOne(GLenum type, const char *src)
{
    const char *parts[2] = { sEs ? "#version 300 es\nprecision mediump float;\n"
                                 : "#version 330 core\n", src };
    GLuint sh = G(glCreateShader)(type);
    GLint ok = 0;

    G(glShaderSource)(sh, 2, parts, NULL);
    G(glCompileShader)(sh);
    G(glGetShaderiv)(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        G(glGetShaderInfoLog)(sh, sizeof(log), NULL, log);
        UiLogError(log);
        return 0;
    }
    return sh;
}

int UiGlInit(int es)
{
    static const char *kVs =
        "layout(location = 0) in vec2 aPos;\n"
        "layout(location = 1) in vec2 aUv;\n"
        "layout(location = 2) in vec4 aCol;\n"
        "uniform mat4 uProj;\n"
        "out vec2 vUv; out vec4 vCol;\n"
        "void main() { vUv = aUv; vCol = aCol; gl_Position = uProj * vec4(aPos, 0.0, 1.0); }\n";
    static const char *kFs =
        "in vec2 vUv; in vec4 vCol;\n"
        "uniform sampler2D uTex;\n"
        "out vec4 oCol;\n"
        "void main() { oCol = vCol * texture(uTex, vUv); }\n";
    GLuint vs, fs;
    GLint ok = 0;

    sEs = es;
    vs = CompileOne(GL_VERTEX_SHADER, kVs);
    fs = CompileOne(GL_FRAGMENT_SHADER, kFs);
    if (!vs || !fs)
        return 0;
    sProg = G(glCreateProgram)();
    G(glAttachShader)(sProg, vs);
    G(glAttachShader)(sProg, fs);
    G(glLinkProgram)(sProg);
    G(glGetProgramiv)(sProg, GL_LINK_STATUS, &ok);
    G(glDeleteShader)(vs);
    G(glDeleteShader)(fs);
    if (!ok) {
        UiLogError("imgui program did not link");
        return 0;
    }
    sLocProj = G(glGetUniformLocation)(sProg, "uProj");
    sLocTex = G(glGetUniformLocation)(sProg, "uTex");
    G(glGenVertexArrays)(1, &sVao);
    G(glGenBuffers)(1, &sVbo);
    G(glGenBuffers)(1, &sIbo);
    G(glBindVertexArray)(sVao);
    G(glBindBuffer)(GL_ARRAY_BUFFER, sVbo);
    G(glEnableVertexAttribArray)(0);
    G(glEnableVertexAttribArray)(1);
    G(glEnableVertexAttribArray)(2);
    G(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                             (const void *)IM_OFFSETOF(ImDrawVert, pos));
    G(glVertexAttribPointer)(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                             (const void *)IM_OFFSETOF(ImDrawVert, uv));
    G(glVertexAttribPointer)(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert),
                             (const void *)IM_OFFSETOF(ImDrawVert, col));
    G(glBindVertexArray)(0);
    return 1;
}

void UiGlRebuildFont(void)
{
    ImGuiIO &io = ImGui::GetIO();
    unsigned char *px;
    int w, h;

    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (sFontTex)
        G(glDeleteTextures)(1, &sFontTex);
    G(glGenTextures)(1, &sFontTex);
    G(glActiveTexture)(GL_TEXTURE0);
    G(glBindTexture)(GL_TEXTURE_2D, sFontTex);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G(glPixelStorei)(GL_UNPACK_ALIGNMENT, 1);
    G(glTexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID((ImTextureID)(intptr_t)sFontTex);
}

void UiGlRender(ImDrawData *dd, int fbW, int fbH)
{
    float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x;
    float T = dd->DisplayPos.y, B = dd->DisplayPos.y + dd->DisplaySize.y;
    const float proj[16] = {
        2.0f / (R - L), 0, 0, 0,
        0, 2.0f / (T - B), 0, 0,
        0, 0, -1.0f, 0,
        (R + L) / (L - R), (T + B) / (B - T), 0, 1.0f,
    };
    ImVec2 clipOff = dd->DisplayPos, clipScale = dd->FramebufferScale;

    if (!sProg || dd->CmdListsCount == 0)
        return;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, 0);
    G(glViewport)(0, 0, fbW, fbH);
    G(glEnable)(GL_BLEND);
    G(glBlendEquation)(GL_FUNC_ADD);
    G(glBlendFuncSeparate)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    G(glDisable)(GL_CULL_FACE);
    G(glDisable)(GL_DEPTH_TEST);
    G(glEnable)(GL_SCISSOR_TEST);
    G(glUseProgram)(sProg);
    G(glUniform1i)(sLocTex, 0);
    G(glUniformMatrix4fv)(sLocProj, 1, GL_FALSE, proj);
    G(glActiveTexture)(GL_TEXTURE0);
    G(glBindVertexArray)(sVao);
    G(glBindBuffer)(GL_ARRAY_BUFFER, sVbo);
    G(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, sIbo);

    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList *cl = dd->CmdLists[n];

        G(glBufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)cl->VtxBuffer.Size * sizeof(ImDrawVert),
                        cl->VtxBuffer.Data, GL_STREAM_DRAW);
        G(glBufferData)(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)cl->IdxBuffer.Size * sizeof(ImDrawIdx),
                        cl->IdxBuffer.Data, GL_STREAM_DRAW);
        for (int c = 0; c < cl->CmdBuffer.Size; c++) {
            const ImDrawCmd *cmd = &cl->CmdBuffer[c];
            float x0 = (cmd->ClipRect.x - clipOff.x) * clipScale.x;
            float y0 = (cmd->ClipRect.y - clipOff.y) * clipScale.y;
            float x1 = (cmd->ClipRect.z - clipOff.x) * clipScale.x;
            float y1 = (cmd->ClipRect.w - clipOff.y) * clipScale.y;

            if (cmd->UserCallback != NULL)
                continue;
            if (x1 <= x0 || y1 <= y0)
                continue;
            G(glScissor)((GLint)x0, (GLint)(fbH - y1), (GLsizei)(x1 - x0), (GLsizei)(y1 - y0));
            G(glBindTexture)(GL_TEXTURE_2D, (GLuint)(intptr_t)cmd->GetTexID());
            G(glDrawElements)(GL_TRIANGLES, (GLsizei)cmd->ElemCount,
                              sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                              (const void *)(intptr_t)(cmd->IdxOffset * sizeof(ImDrawIdx)));
        }
    }
    G(glDisable)(GL_SCISSOR_TEST);
    G(glDisable)(GL_BLEND);
    G(glBindVertexArray)(0);
    G(glBindBuffer)(GL_ARRAY_BUFFER, 0);
    G(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, 0);
    G(glUseProgram)(0);
}
