// renderer.c - OpenGL ES 2.0 renderer with fractional scaling support
#include "compositor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GL_BGRA_EXT might not be defined in all GL headers
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

// Vertex shader - handles fractional scaling transformations
static const char *vertex_shader_source =
    "attribute vec2 position;\n"
    "attribute vec2 texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "uniform mat4 projection;\n"
    "uniform mat4 transform;\n"
    "void main() {\n"
    "    gl_Position = projection * transform * vec4(position, 0.0, 1.0);\n"
    "    v_texcoord = texcoord;\n"
    "}\n";

// Fragment shader - handles texture sampling with proper filtering
static const char *fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "uniform float alpha;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(tex, v_texcoord) * alpha;\n"
    "}\n";

struct triangles_shader {
    GLuint program;
    GLint position_attr;
    GLint texcoord_attr;
    GLint projection_uniform;
    GLint transform_uniform;
    GLint tex_uniform;
    GLint alpha_uniform;
};

static struct triangles_shader shader;

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader_id = glCreateShader(type);
    if (shader_id == 0) {
        fprintf(stderr, "Failed to create shader\n");
        return 0;
    }
    
    glShaderSource(shader_id, 1, &source, NULL);
    glCompileShader(shader_id);
    
    GLint status;
    glGetShaderiv(shader_id, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[1024];
        memset(log, 0, sizeof(log));  // Clear the buffer
        GLsizei length = 0;
        glGetShaderInfoLog(shader_id, sizeof(log) - 1, &length, log);
        fprintf(stderr, "Shader compilation failed (%s shader):\n%s\n", 
                type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                length > 0 ? log : "No error log available");
        glDeleteShader(shader_id);
        return 0;
    }
    
    return shader_id;
}

static bool init_shaders(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    
    if (!vs || !fs) {
        return false;
    }
    
    shader.program = glCreateProgram();
    glAttachShader(shader.program, vs);
    glAttachShader(shader.program, fs);
    glLinkProgram(shader.program);
    
    GLint status;
    glGetProgramiv(shader.program, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(shader.program, sizeof(log), NULL, log);
        fprintf(stderr, "Program linking failed: %s\n", log);
        glDeleteProgram(shader.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    // Get attribute and uniform locations
    shader.position_attr = glGetAttribLocation(shader.program, "position");
    shader.texcoord_attr = glGetAttribLocation(shader.program, "texcoord");
    shader.projection_uniform = glGetUniformLocation(shader.program, "projection");
    shader.transform_uniform = glGetUniformLocation(shader.program, "transform");
    shader.tex_uniform = glGetUniformLocation(shader.program, "tex");
    shader.alpha_uniform = glGetUniformLocation(shader.program, "alpha");
    
    return true;
}

bool triangles_renderer_init(struct triangles_compositor *compositor) {
    // Make sure we have a GL context before doing anything
    if (!eglMakeCurrent(compositor->egl_display, EGL_NO_SURFACE, 
                        EGL_NO_SURFACE, compositor->egl_context)) {
        fprintf(stderr, "Failed to make EGL context current\n");
        return false;
    }
    
    // Check GL version
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("GL Version: %s\n", version);
    printf("GL Vendor: %s\n", vendor);
    printf("GL Renderer: %s\n", renderer);
    
    // Initialize OpenGL
    if (!init_shaders()) {
        fprintf(stderr, "Failed to initialize shaders\n");
        return false;
    }
    
    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    printf("Renderer initialized\n");
    return true;
}

void triangles_renderer_begin(struct triangles_output *output) {
    // Set viewport - this is where fractional scaling matters
    // We render at the full resolution of the output
    glViewport(0, 0, output->width, output->height);
    
    // Clear with a background color
    glClearColor(0.1, 0.1, 0.1, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Use shader program
    glUseProgram(shader.program);
    
    // Set up orthographic projection matrix for 2D rendering
    // This matrix accounts for the output's fractional scale
    float projection[16] = {
        2.0f / output->width, 0, 0, 0,
        0, -2.0f / output->height, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1
    };
    
    glUniformMatrix4fv(shader.projection_uniform, 1, GL_FALSE, projection);
}

void triangles_renderer_render_view(struct triangles_view *view) {
    if (!view || !view->surface || !view->surface->has_buffer) {
        return;
    }
    
    struct triangles_surface *surface = view->surface;
    struct triangles_output *output = view->output;
    
    // Calculate transformed dimensions based on fractional scale
    // This is the key to proper fractional scaling rendering
    double scale_factor = output->scale;
    
    // Transform view coordinates to output coordinates with fractional scaling
    double scaled_x = view->x * scale_factor;
    double scaled_y = view->y * scale_factor;
    double scaled_w = view->width * scale_factor;
    double scaled_h = view->height * scale_factor;
    
    // Account for surface's own scale (for HiDPI buffers)
    if (surface->scale > 1.0) {
        scaled_w = surface->buffer_width / surface->scale * scale_factor;
        scaled_h = surface->buffer_height / surface->scale * scale_factor;
    }
    
    // Set up transformation matrix
    float transform[16] = {
        (float)scaled_w, 0, 0, 0,
        0, (float)scaled_h, 0, 0,
        0, 0, 1, 0,
        (float)scaled_x, (float)scaled_y, 0, 1
    };
    
    glUniformMatrix4fv(shader.transform_uniform, 1, GL_FALSE, transform);
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glUniform1i(shader.tex_uniform, 0);
    
    // Set texture filtering - CRITICAL for fractional scaling
    // Use LINEAR filtering for smooth scaling
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Set alpha
    glUniform1f(shader.alpha_uniform, 1.0f);
    
    // Vertex data: position and texture coordinates
    float vertices[] = {
        // pos      texcoord
        0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
    };
    
    GLushort indices[] = {
        0, 1, 2,
        0, 2, 3,
    };
    
    // Set up vertex attributes
    glVertexAttribPointer(shader.position_attr, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), vertices);
    glVertexAttribPointer(shader.texcoord_attr, 2, GL_FLOAT, GL_FALSE,
                         4 * sizeof(float), vertices + 2);
    
    glEnableVertexAttribArray(shader.position_attr);
    glEnableVertexAttribArray(shader.texcoord_attr);
    
    // Draw
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    
    glDisableVertexAttribArray(shader.position_attr);
    glDisableVertexAttribArray(shader.texcoord_attr);
}

void triangles_renderer_end(struct triangles_output *output) {
    // Finish rendering
    glFinish();
}

// Helper function to create texture from buffer
GLuint triangles_renderer_create_texture(struct triangles_surface *surface,
                                        struct wl_resource *buffer) {
    if (!buffer) {
        printf("[RENDERER] No buffer provided\n");
        fflush(stdout);
        return 0;
    }
    
    // Get the SHM buffer from libwayland
    struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(buffer);
    if (!shm_buffer) {
        fprintf(stderr, "[RENDERER] Not a SHM buffer (might be DMA-BUF)\n");
        fflush(stdout);
        return 0;
    }
    
    // Get buffer properties
    int32_t width = wl_shm_buffer_get_width(shm_buffer);
    int32_t height = wl_shm_buffer_get_height(shm_buffer);
    int32_t stride = wl_shm_buffer_get_stride(shm_buffer);
    uint32_t format = wl_shm_buffer_get_format(shm_buffer);
    
    printf("[RENDERER] Creating texture: %dx%d format=%u stride=%d\n", 
           width, height, format, stride);
    fflush(stdout);
    
    // Map format to GL format
    GLenum gl_format;
    GLenum gl_type = GL_UNSIGNED_BYTE;
    bool needs_swizzle = false;
    
    switch (format) {
        case WL_SHM_FORMAT_ARGB8888:
        case WL_SHM_FORMAT_XRGB8888:
            // ARGB8888 in memory on little-endian is: B G R A (byte order)
            #ifdef GL_BGRA_EXT
                gl_format = GL_BGRA_EXT;
                printf("[RENDERER] Using GL_BGRA_EXT for ARGB format\n");
            #else
                gl_format = GL_RGBA;
                needs_swizzle = true;
                printf("[RENDERER] GL_BGRA not available, will need conversion\n");
            #endif
            break;
        default:
            fprintf(stderr, "[RENDERER] Unsupported buffer format: %u\n", format);
            fflush(stdout);
            return 0;
    }
    
    // Create texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    printf("[RENDERER] Generated texture ID: %u\n", texture);
    fflush(stdout);
    
    // Upload pixel data
    wl_shm_buffer_begin_access(shm_buffer);
    void *data = wl_shm_buffer_get_data(shm_buffer);
    
    if (!data) {
        fprintf(stderr, "[RENDERER] Failed to get buffer data\n");
        wl_shm_buffer_end_access(shm_buffer);
        glDeleteTextures(1, &texture);
        return 0;
    }
    
    // If we need to swizzle (RGBA but data is BGRA), convert it
    void *upload_data = data;
    if (needs_swizzle && gl_format == GL_RGBA) {
        printf("[RENDERER] Converting BGRA to RGBA...\n");
        fflush(stdout);
        
        // Allocate temporary buffer
        upload_data = malloc(height * stride);
        if (upload_data) {
            uint32_t *src = (uint32_t *)data;
            uint32_t *dst = (uint32_t *)upload_data;
            
            for (int i = 0; i < width * height; i++) {
                uint32_t pixel = src[i];
                // BGRA -> RGBA: swap R and B
                dst[i] = (pixel & 0xFF00FF00) |           // Keep G and A
                        ((pixel & 0x00FF0000) >> 16) |    // R to B position
                        ((pixel & 0x000000FF) << 16);     // B to R position
            }
        } else {
            upload_data = data;  // Fallback to original if malloc fails
        }
    }
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                gl_format, GL_UNSIGNED_BYTE, upload_data);
    
    // Free temporary buffer if we allocated one
    if (needs_swizzle && upload_data != data) {
        free(upload_data);
    }
    
    wl_shm_buffer_end_access(shm_buffer);
    
    // Check for GL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "[RENDERER] OpenGL error after texture upload: 0x%x\n", error);
        fflush(stdout);
        glDeleteTextures(1, &texture);
        return 0;
    }
    
    // Set texture parameters - important for fractional scaling
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    printf("[RENDERER] Texture created successfully: %u\n", texture);
    fflush(stdout);
    
    return texture;
}
