// Headless OpenGL render test: G x G quads (4x4 px each) into a 512x512 FBO, read back,
// count triangles actually drawn. Legacy fixed-function (Quake 3 style) and core 4.1 with shaders.
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <OpenGL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
static int g_patch;
// In-process only: patch this process's private copy of the GL driver (updateURBState sku check).
static void patch_gl(void) {
  static const unsigned char f[] = {0x31,0xF6,0x83,0xBB,0x40,0x2F,0,0,0,0x40,0x0F,0x95,0xC6,0xC1,0xE6,0x04,0x83,0xCE,0x0C};
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    if (!strstr(_dyld_get_image_name(i), "KBLGraphicsGLDriver")) continue;
    unsigned long sz; unsigned char *t = getsegmentdata((const struct mach_header_64 *)_dyld_get_image_header(i), "__TEXT", &sz);
    for (unsigned long k = 0; k + sizeof(f) <= sz; k++) if (!memcmp(t + k, f, sizeof(f))) {
      mach_vm_address_t pg = (mach_vm_address_t)(t + k) & ~0xFFFULL;
      kern_return_t kr = mach_vm_protect(mach_task_self(), pg, 0x2000, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
      if (kr) { printf("protect failed %d\n", kr); return; }
      memset(t + k + 9, 0x90, 4);
      mach_vm_protect(mach_task_self(), pg, 0x2000, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
      printf("patched GL updateURBState in-process\n"); return; }
    printf("GL pattern not found (already patched by the kext?)\n"); return; }
  printf("GL driver not loaded\n");
}
enum { W = 512, G = 128 };
static unsigned char px[W*W*4];

static CGLContextObj ctx(int core) {
  CGLPixelFormatAttribute a[] = { kCGLPFAAccelerated, kCGLPFANoRecovery, kCGLPFAAllowOfflineRenderers,
    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)(core ? kCGLOGLPVersion_GL4_Core : kCGLOGLPVersion_Legacy), 0 };
  CGLPixelFormatObj pf; GLint n; CGLContextObj c = NULL;
  if (CGLChoosePixelFormat(a, &pf, &n) || !pf) { printf("no pixel format (core=%d)\n", core); return NULL; }
  CGLCreateContext(pf, NULL, &c); CGLDestroyPixelFormat(pf); CGLSetCurrentContext(c);
  if (g_patch) { patch_gl(); g_patch = 0; } return c;
}
static void fbo(void) {
  GLuint f, rb; glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
  glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, W);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) printf("FBO incomplete\n");
  glViewport(0, 0, W, W);
}
static float *quads(void) {  // 6 verts per quad, x,y in NDC
  float *v = malloc(sizeof(float) * G * G * 12), k[6][2] = {{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
  for (int q = 0; q < G*G; q++) for (int c = 0; c < 6; c++) {
    v[q*12+c*2]   = ((q % G) + k[c][0]) / G * 2 - 1;
    v[q*12+c*2+1] = ((q / G) + k[c][1]) / G * 2 - 1; }
  return v;
}
static double measure(void) {  // % of triangles drawn (sample one pixel inside each half of each quad)
  glFinish(); glReadPixels(0, 0, W, W, GL_RGBA, GL_UNSIGNED_BYTE, px);
  long t = 0; for (int qy = 0; qy < G; qy++) for (int qx = 0; qx < G; qx++) {
    if (px[((qy*4+0)*W + qx*4+1)*4+1] > 200) t++;   // lower-right half (rows count up from bottom in GL)
    if (px[((qy*4+3)*W + qx*4+0)*4+1] > 200) t++; } // upper-left half
  return 100.0 * t / (2.0 * G * G);
}
static void legacy(void) {
  CGLContextObj c = ctx(0); if (!c) return;
  printf("legacy  renderer: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  fbo(); float *v = quads();
  int chunks[] = { G*G, 64, 1 };
  for (int i = 0; i < 3; i++) {
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glColor4f(0,1,0,1);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, v);
    for (int s = 0; s < G*G; s += chunks[i]) glDrawArrays(GL_TRIANGLES, s*6, chunks[i]*6);
    printf("  fixed-function vertex arrays, %5d quads/draw: %5.1f%% triangles drawn\n", chunks[i], measure()); }
  // immediate mode, like old engines
  glClear(GL_COLOR_BUFFER_BIT); glBegin(GL_TRIANGLES);
  for (int j = 0; j < G*G*6; j++) glVertex2f(v[j*2], v[j*2+1]); glEnd();
  printf("  immediate mode glBegin/glEnd             : %5.1f%% triangles drawn\n", measure());
  free(v); CGLSetCurrentContext(NULL); CGLDestroyContext(c);
}
static GLuint prog(int V) {
  char vs[8192], fs[8192], *p; int i;
  p = vs + sprintf(vs, "#version 410\nlayout(location=0) in vec2 pos;\n");
  for (i = 0; i < V; i++) p += sprintf(p, "out vec4 a%d;\n", i);
  p += sprintf(p, "void main(){ gl_Position=vec4(pos,0,1);");
  for (i = 0; i < V; i++) p += sprintf(p, " a%d=vec4(%s);", i, i ? "0.0" : "0,1,0,1");
  sprintf(p, " }\n");
  p = fs + sprintf(fs, "#version 410\n");
  for (i = 0; i < V; i++) p += sprintf(p, "in vec4 a%d;\n", i);
  p += sprintf(p, "out vec4 o; void main(){ o=a0");
  for (i = 1; i < V; i++) p += sprintf(p, "+a%d", i);
  sprintf(p, "; }\n");
  GLuint pr = glCreateProgram(); const char *s[2] = { vs, fs }; GLenum t[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
  for (i = 0; i < 2; i++) { GLuint sh = glCreateShader(t[i]); glShaderSource(sh, 1, &s[i], NULL); glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok); if (!ok) { char l[1024]; glGetShaderInfoLog(sh, 1024, NULL, l); printf("compile: %s\n", l); }
    glAttachShader(pr, sh); }
  glLinkProgram(pr); return pr;
}
static void core(void) {
  CGLContextObj c = ctx(1); if (!c) return;
  printf("core    renderer: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
  fbo(); float *v = quads(); GLuint vao, vbo;
  glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(float) * G * G * 12, v, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  int Vs[] = { 1, 4, 8, 15 };
  for (int i = 0; i < 4; i++) { GLuint pr = prog(Vs[i]); glUseProgram(pr);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT); glDrawArrays(GL_TRIANGLES, 0, G*G*6);
    double one = measure();
    glClear(GL_COLOR_BUFFER_BIT); for (int s = 0; s < G*G; s++) glDrawArrays(GL_TRIANGLES, s*6, 6);
    printf("  shaders, %2d vec4 varyings: one draw %5.1f%%   1 quad/draw %5.1f%% triangles drawn\n", Vs[i], one, measure());
    glDeleteProgram(pr); }
  free(v); CGLSetCurrentContext(NULL); CGLDestroyContext(c);
}
int main(int argc, char **argv) { g_patch = argc > 1 && !strcmp(argv[1], "--patch"); legacy(); core(); return 0; }
