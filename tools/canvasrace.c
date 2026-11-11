/* canvasrace -- exercise Godot 4's canvas instance-buffer recycling and measure
 * barrier cost programmatically. It does not reproduce the device defect and
 * is therefore a throughput microbenchmark, not a correctness oracle.
 *
 * Mirrors RasterizerCanvasGLES3::canvas_render_items(): a ring of instance-data
 * buffers, each guarded by a GLsync polled through glGetSynciv(GL_SYNC_STATUS);
 * when a buffer reads GL_SIGNALED it is refilled with an unsynchronized map and
 * read back by glDrawElementsInstanced in the same frame.
 *
 * Every instance draws a solid quad into its own cell of a grid, coloured from
 * data uploaded for THAT frame. If the GPU is still reading a buffer we have
 * already overwritten, a cell comes out the wrong colour.
 *
 * The check runs ON THE GPU and accumulates. A per-frame glReadPixels would be
 * a full pipeline sync -- it suppresses the very race we are trying to observe
 * (measured: 0 bad frames with no barrier at all). So instead a second pass
 * compares the rendered cells against the expected colours, writes one pass/fail
 * pixel per frame into a strip texture, and the CPU reads that strip exactly
 * once, after the run.
 *
 * The default path now follows Godot 4.3's actual GLES3 canvas upload closely:
 * a 2 MiB GL_ARRAY_BUFFER (16,384 x 128-byte InstanceData), an
 * glMapBufferRange(GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT) upload, eight
 * 16-byte instanced attributes, and indexed instanced draws. Despite Godot's
 * internal "Batch UBO" labels, this data is an ARRAY_BUFFER in 4.3. Set
 * CR_BUFFER=ubo to stress the same 128-byte records through a real uniform
 * buffer and glBindBufferBase instead.
 *
 * MLP1 result (2026-08-19): neither path reproduced the defect. Both completed
 * 2,000 unbarriered frames / 32,000 draws with zero bad frames and no new Mali
 * faults. This rules out the buffer target, record size, allocation size, and
 * unsynchronized upload in isolation; the reproducer is still not an oracle.
 * A bounded fence queue is much cheaper here than glFinish: depth 2 sustained
 * 40.1 fps versus 41.6 with no barrier and 16.6 with per-draw glFinish (300
 * frames, 16 batches, fragment cost 96). All modes reported zero bad frames.
 *
 * env:
 *   CR_BARRIER=0        no barrier (diagnostic; this reproducer stays clean)
 *   CR_BARRIER=1        glFinish after every draw (safe rollback baseline)
 *   CR_BARRIER=N>1      glFinish every Nth draw (disproven experiment)
 *   CR_BARRIER=fence:N  fence+flush every draw; wait N draws behind (default 2)
 *   CR_FRAMES=n         frames to run (default 2000)
 *   CR_BATCHES=n        instanced draw calls per frame (default 16)
 *   CR_FRAGCOST=n       fragment shader iterations, lengthens GPU work (default 96)
 *   CR_BUFFER=attrib    Godot-faithful instance attributes (default)
 *   CR_BUFFER=ubo       optional uniform-buffer stress path
 *   CR_VERBOSE=1        print each bad frame
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define GRID 8                       /* GRID x GRID cells */
#define CELLS (GRID * GRID)
#define FBO_SIZE 512
#define CELL_PX (FBO_SIZE / GRID)
#define POOL_MAX 16
#define MAX_FRAMES 4096              /* width of the results strip */
#define INSTANCE_BYTES 128            /* Godot 4.3 InstanceData */
#define INSTANCE_CAPACITY (16 * 1024) /* Godot's default item_buffer_size */
#define RING_BUFFER_BYTES ((size_t)INSTANCE_BYTES * INSTANCE_CAPACITY)
#define FENCE_QUEUE_MAX 64

static int cfg_barrier, cfg_fence_depth, cfg_frames, cfg_batches, cfg_fragcost, cfg_verbose, cfg_ubo;

static int env_int(const char *name, int dflt)
{
    const char *e = getenv(name);
    return (e && e[0]) ? atoi(e) : dflt;
}

typedef struct {
    GLsync items[FENCE_QUEUE_MAX];
    int head;
    int count;
    unsigned long flushes;
    unsigned long waits;
} FenceQueue;

static void fence_wait_and_delete(GLsync fence)
{
    GLenum result = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_MAX);
    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) {
        fprintf(stderr, "glClientWaitSync incomplete (result=0x%x error=0x%x)\n",
                result, glGetError());
        exit(1);
    }
    glDeleteSync(fence);
}

static void fence_queue_after_draw(FenceQueue *q)
{
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!fence) {
        fprintf(stderr, "glFenceSync failed (0x%x)\n", glGetError());
        exit(1);
    }
    glFlush();
    q->flushes++;

    if (q->count == cfg_fence_depth) {
        fence_wait_and_delete(q->items[q->head]);
        q->head = (q->head + 1) % FENCE_QUEUE_MAX;
        q->count--;
        q->waits++;
    }

    q->items[(q->head + q->count) % FENCE_QUEUE_MAX] = fence;
    q->count++;
}

static void fence_queue_drain(FenceQueue *q)
{
    while (q->count) {
        fence_wait_and_delete(q->items[q->head]);
        q->head = (q->head + 1) % FENCE_QUEUE_MAX;
        q->count--;
        q->waits++;
    }
}

/* Godot 4.3 exposes InstanceData as eight 16-byte attributes (128 bytes). */
typedef struct {
    union {
        float f[8][4];
        uint32_t u[8][4];
    } slot;
} Inst;

_Static_assert(sizeof(Inst) == INSTANCE_BYTES, "InstanceData must remain 128 bytes");

typedef struct {
    GLuint vbo;
    GLsync fence;
    unsigned long last_frame_used;
} Buf;

static Buf pool[POOL_MAX];
static int pool_n;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Colour for (frame, instance) -- distinct per frame so stale data is visible. */
static void expect_color(unsigned long frame, int cell, unsigned char *out)
{
    out[0] = (unsigned char)((frame * 7u + cell * 13u) & 0xFF);
    out[1] = (unsigned char)((frame * 11u + cell * 29u) & 0xFF);
    out[2] = (unsigned char)((frame * 17u + cell * 5u) & 0xFF);
}

static const char *VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=8) in vec4 instance0;\n"
    "layout(location=9) in vec4 instance1;\n"
    "layout(location=10) in vec4 instance2;\n"
    "layout(location=11) in vec4 instance3;\n"
    "layout(location=12) in vec4 instance4;\n"
    "layout(location=13) in vec4 instance5;\n"
    "layout(location=14) in uvec4 instance6;\n"
    "layout(location=15) in uvec4 instance7;\n"
    "out vec3 vcol;\n"
    "void main(){\n"
    "  float cell = instance0.x;\n"
    "  float gx = mod(cell, 8.0), gy = floor(cell / 8.0);\n"
    "  vec2 base = vec2(gx, gy) / 4.0 - 1.0;\n"
    "  gl_Position = vec4(base + (pos + 1.0) * 0.125, 0.0, 1.0);\n"
    "  vcol = (instance0.yzw + instance1.xyz + instance2.xyz + instance3.xyz +\n"
    "          instance4.xyz + instance5.xyz + vec3(instance6.xyz) / 255.0 +\n"
    "          vec3(instance7.xyz) / 255.0) / 8.0;\n"
    "}\n";

static const char *UBO_VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 pos;\n"
    "struct Instance { vec4 slot[8]; };\n"
    "layout(std140) uniform InstanceBlock { Instance instances[64]; };\n"
    "uniform int first_instance;\n"
    "out vec3 vcol;\n"
    "void main(){\n"
    "  vec4 data0 = instances[first_instance + gl_InstanceID].slot[0];\n"
    "  float cell = data0.x;\n"
    "  float gx = mod(cell, 8.0), gy = floor(cell / 8.0);\n"
    "  vec2 base = vec2(gx, gy) / 4.0 - 1.0;\n"
    "  gl_Position = vec4(base + (pos + 1.0) * 0.125, 0.0, 1.0);\n"
    "  vcol = data0.yzw;\n"
    "}\n";

static const char *FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform int cost;\n"
    "in vec3 vcol;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float acc = 0.0;\n"
    "  for (int i = 0; i < cost; i++) { acc += sin(float(i) + gl_FragCoord.x) * 1e-6; }\n"
    "  frag = vec4(vcol + acc, 1.0);\n"
    "}\n";

/* Pass 2: sample every cell of the pass-1 texture and compare against the
 * colour that frame should have produced. Writes 1.0 on any mismatch. */
static const char *CHECK_VS =
    "#version 300 es\n"
    "layout(location=0) in vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *CHECK_FS =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform sampler2D src;\n"
    "uniform int frame;\n"
    "uniform int cellpx;\n"
    "out vec4 frag;\n"
    "float want(int f, int c, int a, int b){\n"
    "  return float((f * a + c * b) % 256) / 255.0;\n"
    "}\n"
    "void main(){\n"
    "  float bad = 0.0;\n"
    "  for (int c = 0; c < 64; c++) {\n"
    "    int gx = c % 8, gy = c / 8;\n"
    "    ivec2 uv = ivec2(gx * cellpx + cellpx / 2, gy * cellpx + cellpx / 2);\n"
    "    vec3 got = texelFetch(src, uv, 0).rgb;\n"
    "    vec3 exp3 = vec3(want(frame, c, 7, 13), want(frame, c, 11, 29), want(frame, c, 17, 5));\n"
    "    if (any(greaterThan(abs(got - exp3), vec3(0.02)))) { bad = 1.0; }\n"
    "  }\n"
    "  frag = vec4(bad, 0.0, 0.0, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        exit(1);
    }
    return s;
}

static void alloc_buffer(int idx, size_t bytes)
{
    glGenBuffers(1, &pool[idx].vbo);
    GLenum target = cfg_ubo ? GL_UNIFORM_BUFFER : GL_ARRAY_BUFFER;
    glBindBuffer(target, pool[idx].vbo);
    glBufferData(target, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
    pool[idx].fence = 0;
    pool[idx].last_frame_used = 0;
}

int main(void)
{
    const char *barrier_mode = getenv("CR_BARRIER");
    if (!barrier_mode || !barrier_mode[0]) barrier_mode = "fence:2";
    cfg_barrier = atoi(barrier_mode);
    if (strncmp(barrier_mode, "fence:", 6) == 0) {
        cfg_fence_depth = atoi(barrier_mode + 6);
        if (cfg_fence_depth < 1) cfg_fence_depth = 1;
        if (cfg_fence_depth > FENCE_QUEUE_MAX) cfg_fence_depth = FENCE_QUEUE_MAX;
    }
    cfg_frames   = env_int("CR_FRAMES", 2000);
    cfg_batches  = env_int("CR_BATCHES", 16);
    cfg_fragcost = env_int("CR_FRAGCOST", 96);
    cfg_verbose  = env_int("CR_VERBOSE", 0);
    const char *buffer_mode = getenv("CR_BUFFER");
    cfg_ubo = buffer_mode && strcmp(buffer_mode, "ubo") == 0;
    if (cfg_batches < 1) cfg_batches = 1;
    if (cfg_frames < 1) cfg_frames = 1;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); return 1; }
    if (!eglInitialize(dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed 0x%x\n", eglGetError()); return 1; }

    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no config\n"); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    EGLint pbattr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbattr);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "makeCurrent failed\n"); return 1; }

    printf("renderer : %s\n", glGetString(GL_RENDERER));
    printf("version  : %s\n", glGetString(GL_VERSION));
    if (cfg_fence_depth) {
        printf("barrier  : fence+flush every draw, wait %d draws behind\n", cfg_fence_depth);
    } else {
        printf("barrier  : %s\n",
               cfg_barrier == 0 ? "none" : cfg_barrier == 1 ? "every draw" : "every Nth draw");
        if (cfg_barrier > 1) printf("           N = %d\n", cfg_barrier);
    }
    printf("buffer   : %s, %.1f MiB each, %d-byte records, unsynchronized map\n",
           cfg_ubo ? "uniform" : "attributes", RING_BUFFER_BYTES / (1024.0 * 1024.0),
           INSTANCE_BYTES);
    printf("frames=%d batches/frame=%d instances/batch=%d fragcost=%d\n\n",
           cfg_frames, cfg_batches, CELLS / cfg_batches ? CELLS / cfg_batches : 1, cfg_fragcost);

    /* Offscreen target: no compositor, no vsync, nothing to throttle us. */
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, FBO_SIZE, FBO_SIZE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "fbo incomplete\n"); return 1;
    }
    glViewport(0, 0, FBO_SIZE, FBO_SIZE);

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, cfg_ubo ? UBO_VS : VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) { fprintf(stderr, "link failed\n"); return 1; }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "cost"), cfg_fragcost);
    GLint first_instance_uniform = -1;
    if (cfg_ubo) {
        GLuint block = glGetUniformBlockIndex(prog, "InstanceBlock");
        if (block == GL_INVALID_INDEX) {
            fprintf(stderr, "InstanceBlock missing\n");
            return 1;
        }
        glUniformBlockBinding(prog, block, 0);
        first_instance_uniform = glGetUniformLocation(prog, "first_instance");
    }

    GLuint chk = glCreateProgram();
    glAttachShader(chk, compile(GL_VERTEX_SHADER, CHECK_VS));
    glAttachShader(chk, compile(GL_FRAGMENT_SHADER, CHECK_FS));
    glLinkProgram(chk);
    glGetProgramiv(chk, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(chk, sizeof(log), NULL, log);
        fprintf(stderr, "check link failed: %s\n", log);
        return 1;
    }

    /* Results strip: one pixel per frame, read back once at the end. */
    if (cfg_frames > MAX_FRAMES) cfg_frames = MAX_FRAMES;
    GLuint acc_tex, acc_fbo;
    glGenTextures(1, &acc_tex);
    glBindTexture(GL_TEXTURE_2D, acc_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, MAX_FRAMES, 1);
    glGenFramebuffers(1, &acc_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, acc_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, acc_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "accum fbo incomplete\n"); return 1;
    }
    /* Red means failure. Start failed so a dropped validation draw cannot be
     * mistaken for a clean frame; a successful check draw overwrites this. */
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* Same winding/order as Godot's indexed canvas quad. */
    static const float quad[] = { -1,-1, -1,1, 1,1, 1,-1 };
    GLuint vao_draw, vao_chk, quadbuf, indexbuf;
    glGenBuffers(1, &quadbuf);
    glBindBuffer(GL_ARRAY_BUFFER, quadbuf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao_chk);
    glBindVertexArray(vao_chk);
    glBindBuffer(GL_ARRAY_BUFFER, quadbuf);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

    glGenVertexArrays(1, &vao_draw);
    glBindVertexArray(vao_draw);
    glBindBuffer(GL_ARRAY_BUFFER, quadbuf);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    static const GLuint quad_indices[] = { 0, 2, 1, 3, 2, 0 };
    glGenBuffers(1, &indexbuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexbuf);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_indices), quad_indices, GL_STATIC_DRAW);

    const size_t inst_bytes = sizeof(Inst) * CELLS;
    pool_n = 3;                       /* Godot's default ring depth */
    for (int i = 0; i < pool_n; i++) alloc_buffer(i, RING_BUFFER_BYTES);

    Inst *staging = malloc(inst_bytes);
    unsigned long draws = 0, finishes = 0;
    FenceQueue draw_fences = { 0 };
    double t0 = now_s();

    for (unsigned long f = 1; f <= (unsigned long)cfg_frames; f++) {
        int idx = (int)(f % (unsigned long)pool_n);

        /* --- Godot's fence check, mirrored in structure ------------------- */
        if (pool[idx].fence) {
            GLint status = 0;
            glGetSynciv(pool[idx].fence, GL_SYNC_STATUS, 1, NULL, &status);
            if (status == GL_UNSIGNALED) {
                if (pool[idx].last_frame_used < f - 2) {
                    glClientWaitSync(pool[idx].fence, 0, 100000000);
                    glDeleteSync(pool[idx].fence);
                    pool[idx].fence = 0;
                } else if (pool_n < POOL_MAX) {
                    idx = pool_n;                 /* grow the ring, as Godot does */
                    alloc_buffer(pool_n, RING_BUFFER_BYTES);
                    pool_n++;
                }
            } else {
                glDeleteSync(pool[idx].fence);
                pool[idx].fence = 0;
            }
        }
        pool[idx].last_frame_used = f;

        for (int c = 0; c < CELLS; c++) {
            unsigned char col[3];
            expect_color(f, c, col);
            memset(&staging[c], 0, sizeof(staging[c]));
            staging[c].slot.f[0][0] = (float)c;
            staging[c].slot.f[0][1] = col[0] / 255.0f;
            staging[c].slot.f[0][2] = col[1] / 255.0f;
            staging[c].slot.f[0][3] = col[2] / 255.0f;
            for (int s = 1; s <= 5; s++) {
                staging[c].slot.f[s][0] = col[0] / 255.0f;
                staging[c].slot.f[s][1] = col[1] / 255.0f;
                staging[c].slot.f[s][2] = col[2] / 255.0f;
            }
            for (int s = 6; s <= 7; s++) {
                staging[c].slot.u[s][0] = col[0];
                staging[c].slot.u[s][1] = col[1];
                staging[c].slot.u[s][2] = col[2];
            }
        }

        /* --- pass 1: draw the grid from the recycled buffer ---------------- */
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, FBO_SIZE, FBO_SIZE);
        glUseProgram(prog);
        glBindVertexArray(vao_draw);

        GLenum target = cfg_ubo ? GL_UNIFORM_BUFFER : GL_ARRAY_BUFFER;
        glBindBuffer(target, pool[idx].vbo);
        void *mapped = glMapBufferRange(target, 0, (GLsizeiptr)inst_bytes,
                                        GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        if (!mapped) {
            fprintf(stderr, "glMapBufferRange failed at frame %lu (0x%x)\n", f, glGetError());
            return 1;
        }
        memcpy(mapped, staging, inst_bytes);
        if (!glUnmapBuffer(target)) {
            fprintf(stderr, "glUnmapBuffer reported corrupt data at frame %lu\n", f);
            return 1;
        }
        if (cfg_ubo) glBindBufferBase(GL_UNIFORM_BUFFER, 0, pool[idx].vbo);

        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        int per_batch = CELLS / cfg_batches;
        if (per_batch < 1) per_batch = 1;
        for (int b = 0; b * per_batch < CELLS; b++) {
            int first = b * per_batch;
            int count = (first + per_batch > CELLS) ? (CELLS - first) : per_batch;
            if (cfg_ubo) {
                glUniform1i(first_instance_uniform, first);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, pool[idx].vbo);
                for (int a = 8; a <= 15; a++) {
                    glEnableVertexAttribArray((GLuint)a);
                    size_t offset = (size_t)(first * sizeof(Inst) +
                                             (a - 8) * 4 * sizeof(float));
                    if (a < 14) {
                        glVertexAttribPointer((GLuint)a, 4, GL_FLOAT, GL_FALSE,
                                              sizeof(Inst), (void *)offset);
                    } else {
                        glVertexAttribIPointer((GLuint)a, 4, GL_UNSIGNED_INT,
                                               sizeof(Inst), (void *)offset);
                    }
                    glVertexAttribDivisor((GLuint)a, 1);
                }
            }
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL, count);
            draws++;
            if (cfg_fence_depth) { fence_queue_after_draw(&draw_fences); }
            else if (cfg_barrier == 1) { glFinish(); finishes++; }
            else if (cfg_barrier > 1 && (draws % (unsigned long)cfg_barrier) == 0) { glFinish(); finishes++; }
        }

        pool[idx].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

        /* --- pass 2: GPU-side check, one result pixel per frame ------------ */
        glBindFramebuffer(GL_FRAMEBUFFER, acc_fbo);
        glViewport((GLint)(f - 1), 0, 1, 1);
        glUseProgram(chk);
        glBindVertexArray(vao_chk);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(chk, "src"), 0);
        glUniform1i(glGetUniformLocation(chk, "frame"), (GLint)(f % 256u));
        glUniform1i(glGetUniformLocation(chk, "cellpx"), CELL_PX);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    if (cfg_fence_depth) fence_queue_drain(&draw_fences);

    /* --- single readback, after the run ---------------------------------- */
    glBindFramebuffer(GL_FRAMEBUFFER, acc_fbo);
    unsigned char *strip = malloc((size_t)MAX_FRAMES * 4);
    glReadPixels(0, 0, MAX_FRAMES, 1, GL_RGBA, GL_UNSIGNED_BYTE, strip);
    double dt = now_s() - t0;

    unsigned long bad_frames = 0, first_bad = 0;
    for (int i = 0; i < cfg_frames; i++) {
        if (strip[(size_t)i * 4] > 128) {
            bad_frames++;
            if (!first_bad) first_bad = (unsigned long)i + 1;
            if (cfg_verbose && bad_frames <= 20) printf("  bad frame %d\n", i + 1);
        }
    }

    printf("frames        : %d\n", cfg_frames);
    printf("draw calls    : %lu\n", draws);
    printf("glFinish calls: %lu\n", finishes);
    printf("glFlush calls : %lu\n", draw_fences.flushes);
    printf("fence waits   : %lu\n", draw_fences.waits);
    printf("pool grew to  : %d buffers\n", pool_n);
    printf("bad frames    : %lu  (%.2f%%)\n", bad_frames, 100.0 * bad_frames / cfg_frames);
    if (bad_frames) printf("first bad     : frame %lu\n", first_bad);
    printf("elapsed       : %.2fs   (%.1f frames/s)\n", dt, cfg_frames / dt);
    printf("VERDICT       : %s\n", bad_frames ? "CORRUPTION DETECTED" : "clean");
    return bad_frames ? 2 : 0;
}
