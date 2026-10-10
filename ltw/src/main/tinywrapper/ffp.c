/**
 * Fixed-function pipeline (compatibility) emulation for the OpenGL ES wrapper.
 * This module implements enough of the OpenGL 1.x/2.0 fixed-function pipeline
 * to run classic desktop GL applications on top of the usual GLES3 backend.
 *
 * For use under LGPL-3.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "ffp.h"
#include "egl.h"
#include "proc.h"
#include "env.h"

#define GL_GLEXT_PROTOTYPES
#include "GL/gl.h"
#include "GL/glext.h"

#ifndef FFP_PI
#define FFP_PI 3.14159265358979323846f
#endif

#ifndef GL_CONTEXT_COMPATIBILITY_PROFILE_BIT
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x00000002
#endif

#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#ifndef GL_QUAD_STRIP
#define GL_QUAD_STRIP 0x0008
#endif
#ifndef GL_POLYGON
#define GL_POLYGON 0x0009
#endif

#define FFP_MAX_LIGHTS 8
#define FFP_MAX_TEX 4
#define FFP_MAX_CLIP 6
#define FFP_MAX_MATRIX_STACK 32
#define FFP_MAX_ATTRIB_STACK 16

/* Attribute locations used by the generated fixed-function program. */
#define FFP_LOC_POSITION   0
#define FFP_LOC_NORMAL     1
#define FFP_LOC_COLOR      2
#define FFP_LOC_SECONDARY  3
#define FFP_LOC_FOGCOORD   4
#define FFP_LOC_TEXCOORD0  5 /* + unit */

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const void* ptr;
} ffp_pointer_t;

typedef struct {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
} ffp_material_t;

typedef struct {
    int enabled[FFP_MAX_LIGHTS];
    int spotlight[FFP_MAX_LIGHTS];
    float position[FFP_MAX_LIGHTS][4];
    float ambient[FFP_MAX_LIGHTS][4];
    float diffuse[FFP_MAX_LIGHTS][4];
    float specular[FFP_MAX_LIGHTS][4];
    float spot_dir[FFP_MAX_LIGHTS][3];
    float spot_cos[FFP_MAX_LIGHTS];
    float spot_exp[FFP_MAX_LIGHTS];
    float atten[FFP_MAX_LIGHTS][3];
    float model_ambient[4];
    int two_side;
    int local_viewer;
} ffp_light_state_t;

typedef struct {
    int enabled[FFP_MAX_TEX];
    int target[FFP_MAX_TEX];     /* 0 = 2D, 1 = cube */
    int env_mode[FFP_MAX_TEX];
    float env_color[FFP_MAX_TEX][4];
    int gen_mode[4][FFP_MAX_TEX];          /* [s/t/r/q][unit] */
    float gen_object[4][FFP_MAX_TEX][4];   /* [comp][unit][4] */
    float gen_eye[4][FFP_MAX_TEX][4];
    float matrix[FFP_MAX_TEX][16];
} ffp_texture_state_t;

typedef struct {
    /* matrices */
    int matrix_mode;
    float modelview[16];
    float projection[16];
    float texture[FFP_MAX_TEX][16];
    float modelview_stack[FFP_MAX_MATRIX_STACK][16];
    int modelview_top;
    float projection_stack[FFP_MAX_MATRIX_STACK][16];
    int projection_top;
    float texture_stack[FFP_MAX_TEX][FFP_MAX_MATRIX_STACK][16];
    int texture_top[FFP_MAX_TEX];

    /* current vertex state */
    float current_color[4];
    float current_normal[3];
    float current_secondary[4];
    float current_texcoord[FFP_MAX_TEX][4];
    float current_fogcoord;
    unsigned int current_edgeflag;

    /* client arrays */
    ffp_pointer_t vertex_array;
    ffp_pointer_t normal_array;
    ffp_pointer_t color_array;
    ffp_pointer_t secondary_array;
    ffp_pointer_t fog_array;
    ffp_pointer_t texcoord_array[FFP_MAX_TEX];
    int client_active_texture;
    int active_texture;

    /* lighting */
    int lighting;
    int num_lights;
    int color_material;
    int color_material_face;
    int color_material_mode;
    int shade_model; /* 0 = flat, 1 = smooth */
    int color_sum;
    int normalize;
    int rescale_normal;
    float rescale_factor;
    ffp_material_t material_front;
    ffp_material_t material_back;
    ffp_light_state_t light;

    /* fog */
    int fog_enabled;
    int fog_mode; /* 0 = linear, 1 = exp, 2 = exp2 */
    float fog_color[4];
    float fog_start;
    float fog_end;
    float fog_density;
    int fog_coord_src; /* 1 = GL_FOG_COORD */

    /* alpha test */
    int alpha_test;
    int alpha_func;
    float alpha_ref;

    /* textures */
    ffp_texture_state_t tex;

    /* clip planes */
    int clip_enabled[FFP_MAX_CLIP];
    float clip_plane[FFP_MAX_CLIP][4];
    float clip_plane_object[FFP_MAX_CLIP][4];
    int num_clip_planes;

    /* fixed function misc caps (tracked but largely ignored) */
    int line_smooth;
    int polygon_smooth;
    int polygon_stipple;

    /* immediate mode */
    int in_begin_marker;
    GLenum begin_mode;
} ffp_state_t;

typedef struct {
    float position[4];
    float normal[3];
    float color[4];
    float secondary[4];
    float fogcoord;
    float texcoord[FFP_MAX_TEX][4];
} ffp_vert_t;

/* Display list command. */
typedef struct {
    int op;
    int i0, i1, i2;
    float f[8];
} ffp_cmd_t;

typedef struct {
    bool used;
    ffp_cmd_t* cmds;
    int count;
    int capacity;
} ffp_dlist_t;

typedef struct {
    GLint modelView;
    GLint projection;
    GLint normalMatrix;
    GLint normalize;
    GLint lighting;
    GLint numLights;
    GLint lightEnabled;
    GLint lightSpot;
    GLint lightPosition;
    GLint lightAmbient;
    GLint lightDiffuse;
    GLint lightSpecular;
    GLint lightSpotDir;
    GLint lightSpotCos;
    GLint lightSpotExp;
    GLint lightAtten;
    GLint matAmbient;
    GLint matDiffuse;
    GLint matSpecular;
    GLint matEmission;
    GLint matShininess;
    GLint globalAmbient;
    GLint localViewer;
    GLint colorMaterial;
    GLint colorMaterialMode;
    GLint colorSum;
    GLint fogEnabled;
    GLint fogCoordEnabled;
    GLint fogMode;
    GLint fogStart;
    GLint fogEnd;
    GLint fogDensity;
    GLint fogColor;
    GLint texEnabled;
    GLint texTarget;
    GLint texMatrix;
    GLint texGenS;
    GLint texGenT;
    GLint texGenR;
    GLint texGenObjS;
    GLint texGenObjT;
    GLint texGenObjR;
    GLint texGenEyeS;
    GLint texGenEyeT;
    GLint texGenEyeR;
    GLint envMode;
    GLint envColor;
    GLint alphaTest;
    GLint alphaFunc;
    GLint alphaRef;
    GLint numClipPlanes;
    GLint clipPlanes;
    GLint tex2D[FFP_MAX_TEX];
    GLint texCube[FFP_MAX_TEX];
} ffp_uniforms_t;

typedef struct {
    ffp_state_t state;
    bool ff_in_use;
    GLuint program;
    bool program_failed;

    GLuint imm_vbo;
    ffp_vert_t* imm_verts;
    int imm_count;
    int imm_capacity;

    GLuint quad_ibo;
    GLuint* idx_scratch;
    int idx_capacity;

    ffp_uniforms_t loc;
    ffp_dlist_t* dlists;
    int dlists_capacity;

    bool recording;
    int record_list;

    ffp_state_t* attrib_stack;
    int attrib_stack_top;
    int attrib_stack_capacity;
    ffp_state_t* client_stack;
    int client_stack_top;
    int client_stack_capacity;
} ffp_context_t;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

#define FFP (current_context ? (ffp_context_t*)current_context->ffp : NULL)
#define FFP_MARK(c) do { if (c) (c)->ff_in_use = true; } while (0)

static bool g_compat = false;

bool ffp_compat_enabled(void) {
    return g_compat;
}

static float ffp_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void mat4_identity(float* m) {
    int i;
    for (i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_copy(float* dst, const float* src) {
    memcpy(dst, src, sizeof(float) * 16);
}

static void mat4_mul(float* out, const float* a, const float* b) {
    /* out = a * b (column major) */
    float t[16];
    int c, r;
    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            t[c * 4 + r] =
                a[0 * 4 + r] * b[c * 4 + 0] +
                a[1 * 4 + r] * b[c * 4 + 1] +
                a[2 * 4 + r] * b[c * 4 + 2] +
                a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    mat4_copy(out, t);
}

static void mat4_transpose(float* out, const float* a) {
    float t[16];
    int c, r;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            t[r * 4 + c] = a[c * 4 + r];
    mat4_copy(out, t);
}

/* Compute the normal matrix = transpose(inverse(upper-left 3x3 of m)). */
static void mat3_normal_matrix(const float* m, float* out) {
    /* Build a row-major 3x3 so the classic inverse formula applies directly,
     * then store the inverse as column-major which yields inverse-transpose. */
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h;
    float B = -(d * i - f * g);
    float C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float inv[9];
    if (fabsf(det) < 1e-12f) {
        memset(out, 0, sizeof(float) * 9);
        out[0] = out[4] = out[8] = 1.0f;
        return;
    }
    det = 1.0f / det;
    /* inverse in row-major order (index = row*3 + col) */
    inv[0] = A * det;
    inv[1] = -(b * i - c * h) * det;
    inv[2] = (b * f - c * e) * det;
    inv[3] = B * det;
    inv[4] = (a * i - c * g) * det;
    inv[5] = -(a * f - c * d) * det;
    inv[6] = C * det;
    inv[7] = -(a * h - b * g) * det;
    inv[8] = (a * e - b * d) * det;
    /* Store as column-major (index = col*3 + row) => transpose. */
    out[0] = inv[0]; out[1] = inv[3]; out[2] = inv[6];
    out[3] = inv[1]; out[4] = inv[4]; out[5] = inv[7];
    out[6] = inv[2]; out[7] = inv[5]; out[8] = inv[8];
}

static void mat4_ortho(float* m, double l, double r, double b, double t, double n, double f) {
    float rl = (float)(r - l), tb = (float)(t - b), fn = (float)(f - n);
    mat4_identity(m);
    m[0] = 2.0f / rl;
    m[5] = 2.0f / tb;
    m[10] = -2.0f / fn;
    m[12] = -(float)(r + l) / rl;
    m[13] = -(float)(t + b) / tb;
    m[14] = -(float)(f + n) / fn;
    m[15] = 1.0f;
}

static void mat4_frustum(float* m, double l, double r, double b, double t, double n, double f) {
    float rl = (float)(r - l), tb = (float)(t - b), fn = (float)(f - n);
    memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f * (float)n / rl;
    m[5] = 2.0f * (float)n / tb;
    m[8] = (float)(r + l) / rl;
    m[9] = (float)(t + b) / tb;
    m[10] = -(float)(f + n) / fn;
    m[11] = -1.0f;
    m[14] = -2.0f * (float)f * (float)n / fn;
}

/* Multiply m by a translation/scale/rotation matrix (m = m * T). */
static void mat4_apply(float* m, const float* n) {
    float t[16];
    mat4_mul(t, m, n);
    mat4_copy(m, t);
}

static void state_default(ffp_state_t* s) {
    int i, u;
    memset(s, 0, sizeof(*s));
    s->matrix_mode = GL_MODELVIEW;
    mat4_identity(s->modelview);
    mat4_identity(s->projection);
    for (u = 0; u < FFP_MAX_TEX; u++) mat4_identity(s->texture[u]);
    s->current_color[0] = s->current_color[1] = s->current_color[2] = s->current_color[3] = 1.0f;
    s->current_normal[2] = 1.0f;
    s->current_secondary[3] = 1.0f;
    for (u = 0; u < FFP_MAX_TEX; u++) s->current_texcoord[u][3] = 1.0f;
    s->material_front.ambient[3] = 1.0f;
    s->material_front.diffuse[3] = 1.0f;
    s->material_front.specular[3] = 1.0f;
    s->material_front.emission[3] = 1.0f;
    s->material_front.shininess = 0.0f;
    s->material_back = s->material_front;
    s->material_front.ambient[0] = s->material_front.ambient[1] = s->material_front.ambient[2] = 0.2f;
    s->material_back.ambient[0] = s->material_back.ambient[1] = s->material_back.ambient[2] = 0.2f;
    s->material_front.diffuse[0] = s->material_front.diffuse[1] = s->material_front.diffuse[2] = 0.8f;
    s->material_back.diffuse[0] = s->material_back.diffuse[1] = s->material_back.diffuse[2] = 0.8f;
    s->material_front.specular[0] = s->material_front.specular[1] = s->material_front.specular[2] = 0.0f;
    s->material_back.specular[0] = s->material_back.specular[1] = s->material_back.specular[2] = 0.0f;
    s->light.model_ambient[0] = s->light.model_ambient[1] = s->light.model_ambient[2] = 0.2f;
    s->light.model_ambient[3] = 1.0f;
    for (i = 0; i < FFP_MAX_LIGHTS; i++) {
        s->light.position[i][0] = 0.0f;
        s->light.position[i][1] = 0.0f;
        s->light.position[i][2] = 1.0f;
        s->light.position[i][3] = 0.0f;
        s->light.diffuse[i][0] = s->light.diffuse[i][1] = s->light.diffuse[i][2] = s->light.diffuse[i][3] = (i == 0) ? 1.0f : 0.0f;
        s->light.specular[i][0] = s->light.specular[i][1] = s->light.specular[i][2] = s->light.specular[i][3] = (i == 0) ? 1.0f : 0.0f;
        s->light.ambient[i][3] = 1.0f;
        s->light.spot_dir[i][0] = 0.0f;
        s->light.spot_dir[i][1] = 0.0f;
        s->light.spot_dir[i][2] = -1.0f;
        s->light.spot_cos[i] = -1.0f;
        s->light.spot_exp[i] = 0.0f;
        s->light.atten[i][0] = 1.0f;
        s->light.atten[i][1] = 0.0f;
        s->light.atten[i][2] = 0.0f;
    }
    s->fog_mode = GL_LINEAR;
    s->fog_color[0] = s->fog_color[1] = s->fog_color[2] = 0.0f;
    s->fog_color[3] = 1.0f;
    s->fog_start = 0.0f;
    s->fog_end = 1.0f;
    s->fog_density = 1.0f;
    s->alpha_func = GL_ALWAYS;
    s->alpha_ref = 0.0f;
    s->shade_model = GL_SMOOTH;
    s->active_texture = GL_TEXTURE0;
    s->client_active_texture = GL_TEXTURE0;
    s->color_material_mode = GL_AMBIENT_AND_DIFFUSE;
    for (u = 0; u < FFP_MAX_TEX; u++) {
        s->tex.env_mode[u] = GL_MODULATE;
        s->tex.env_color[u][0] = s->tex.env_color[u][1] = s->tex.env_color[u][2] = s->tex.env_color[u][3] = 0.0f;
        s->tex.target[u] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Shader                                                                    */
/* ------------------------------------------------------------------------- */

static const char* FFP_VERTEX_SHADER =
"#version 300 es\n"
"precision highp float;\n"
"const int MAX_LIGHTS = 8;\n"
"const int MAX_TEX = 4;\n"
"uniform mat4 uModelView;\n"
"uniform mat4 uProjection;\n"
"uniform mat3 uNormalMatrix;\n"
"uniform bool uNormalize;\n"
"uniform bool uLighting;\n"
"uniform int uNumLights;\n"
"uniform bool uLightEnabled[MAX_LIGHTS];\n"
"uniform bool uLightSpot[MAX_LIGHTS];\n"
"uniform vec4 uLightPosition[MAX_LIGHTS];\n"
"uniform vec4 uLightAmbient[MAX_LIGHTS];\n"
"uniform vec4 uLightDiffuse[MAX_LIGHTS];\n"
"uniform vec4 uLightSpecular[MAX_LIGHTS];\n"
"uniform vec3 uLightSpotDir[MAX_LIGHTS];\n"
"uniform float uLightSpotCos[MAX_LIGHTS];\n"
"uniform float uLightSpotExp[MAX_LIGHTS];\n"
"uniform vec3 uLightAtten[MAX_LIGHTS];\n"
"uniform vec4 uMatAmbient;\n"
"uniform vec4 uMatDiffuse;\n"
"uniform vec4 uMatSpecular;\n"
"uniform vec4 uMatEmission;\n"
"uniform float uMatShininess;\n"
"uniform vec4 uGlobalAmbient;\n"
"uniform bool uLocalViewer;\n"
"uniform bool uColorMaterial;\n"
"uniform int uColorMaterialMode;\n"
"uniform bool uColorSum;\n"
"uniform bool uFogEnabled;\n"
"uniform bool uFogCoordEnabled;\n"
"uniform bool uTexEnabled[MAX_TEX];\n"
"uniform mat4 uTexMatrix[MAX_TEX];\n"
"uniform int uTexGenS[MAX_TEX];\n"
"uniform int uTexGenT[MAX_TEX];\n"
"uniform int uTexGenR[MAX_TEX];\n"
"uniform vec4 uTexGenObjS[MAX_TEX];\n"
"uniform vec4 uTexGenObjT[MAX_TEX];\n"
"uniform vec4 uTexGenObjR[MAX_TEX];\n"
"uniform vec4 uTexGenEyeS[MAX_TEX];\n"
"uniform vec4 uTexGenEyeT[MAX_TEX];\n"
"uniform vec4 uTexGenEyeR[MAX_TEX];\n"
"uniform int uNumClipPlanes;\n"
"uniform vec4 uClipPlanes[6];\n"
"in vec4 aPosition;\n"
"in vec3 aNormal;\n"
"in vec4 aColor;\n"
"in vec4 aSecondaryColor;\n"
"in float aFogCoord;\n"
"in vec4 aTexCoord0;\n"
"in vec4 aTexCoord1;\n"
"in vec4 aTexCoord2;\n"
"in vec4 aTexCoord3;\n"
"out vec4 vColor;\n"
"out vec4 vSecondary;\n"
"out float vFogFragCoord;\n"
"out vec4 vTexCoord[MAX_TEX];\n"
"out float vClipDist[6];\n"
"float genOne(int comp, int mode, vec4 objPlane, vec4 eyePlane, vec4 objPos, vec4 eyePos, vec3 N, vec3 view) {\n"
"  if (mode == 1) return dot(objPlane, objPos);\n"
"  if (mode == 2) return dot(eyePlane, eyePos);\n"
"  vec3 r = reflect(-view, N);\n"
"  if (mode == 3) {\n"
"    float m = 2.0 * sqrt(max(r.x*r.x + r.y*r.y + (r.z+1.0)*(r.z+1.0), 1e-6));\n"
"    if (comp == 0) return r.x / m + 0.5;\n"
"    if (comp == 1) return r.y / m + 0.5;\n"
"    return 0.0;\n"
"  }\n"
"  if (mode == 4 || mode == 5) {\n"
"    if (comp == 0) return r.x;\n"
"    if (comp == 1) return r.y;\n"
"    return r.z;\n"
"  }\n"
"  return 0.0;\n"
"}\n"
"void main() {\n"
"  vec4 objPos = aPosition;\n"
"  vec4 eyePos = uModelView * objPos;\n"
"  vec3 N = uNormalMatrix * aNormal;\n"
"  N = uNormalize ? normalize(N) : N;\n"
"  vec4 color = aColor;\n"
"  vec4 matAmb = uMatAmbient;\n"
"  vec4 matDif = uMatDiffuse;\n"
"  vec4 matSpec = uMatSpecular;\n"
"  vec4 matEmis = uMatEmission;\n"
"  if (uColorMaterial) {\n"
"    if (uColorMaterialMode == 0x1200) matAmb = color;\n"
"    else if (uColorMaterialMode == 0x1201) matDif = color;\n"
"    else if (uColorMaterialMode == 0x1202) matSpec = color;\n"
"    else if (uColorMaterialMode == 0x1600) matEmis = color;\n"
"    else { matAmb = color; matDif = color; }\n"
"  }\n"
"  vec4 outColor;\n"
"  if (uLighting) {\n"
"    vec3 P = eyePos.xyz;\n"
"    vec3 V = uLocalViewer ? normalize(-P) : vec3(0.0, 0.0, 1.0);\n"
"    vec3 Nn = normalize(N);\n"
"    vec4 accum = uGlobalAmbient * matAmb + matEmis;\n"
"#define LIGHT_BLOCK(I) \\\n"
"    if (I < uNumLights && uLightEnabled[I]) { \\\n"
"      vec4 lp = uLightPosition[I]; \\\n"
"      vec3 L; float atten = 1.0; \\\n"
"      if (lp.w == 0.0) { L = normalize(lp.xyz); } \\\n"
"      else { vec3 d = lp.xyz - P; float dist = length(d); \\\n"
"        L = d / max(dist, 1e-6); \\\n"
"        atten = 1.0 / max(dot(uLightAtten[I], vec3(1.0, dist, dist*dist)), 1e-6); } \\\n"
"      if (uLightSpot[I]) { float spot = dot(normalize(-L), normalize(uLightSpotDir[I])); \\\n"
"        if (spot < uLightSpotCos[I]) atten = 0.0; \\\n"
"        else atten *= pow(max(spot, 0.0), uLightSpotExp[I]); } \\\n"
"      float ndl = max(dot(Nn, L), 0.0); \\\n"
"      accum += (uLightAmbient[I] * matAmb + uLightDiffuse[I] * matDif * ndl) * atten; \\\n"
"      if (ndl > 0.0) { vec3 H = normalize(L + V); \\\n"
"        float spec = pow(max(dot(Nn, H), 0.0), max(uMatShininess, 1e-4)); \\\n"
"        accum.rgb += (uLightSpecular[I].rgb * matSpec.rgb * spec) * atten; } \\\n"
"    }\n"
"  LIGHT_BLOCK(0)\n"
"  LIGHT_BLOCK(1)\n"
"  LIGHT_BLOCK(2)\n"
"  LIGHT_BLOCK(3)\n"
"  LIGHT_BLOCK(4)\n"
"  LIGHT_BLOCK(5)\n"
"  LIGHT_BLOCK(6)\n"
"  LIGHT_BLOCK(7)\n"
"#undef LIGHT_BLOCK\n"
"    accum.a = matDif.a;\n"
"    outColor = accum;\n"
"  } else {\n"
"    outColor = color;\n"
"  }\n"
"  vColor = clamp(outColor, 0.0, 1.0);\n"
"  vSecondary = uColorSum ? aSecondaryColor : vec4(0.0);\n"
"  vec3 viewDir = normalize(-eyePos.xyz);\n"
"  vec3 Nn_l = normalize(N);\n"
"  vec4 tc;\n"
"  tc = aTexCoord0;\n"
"  if (uTexGenS[0] != 0) tc.x = genOne(0, uTexGenS[0], uTexGenObjS[0], uTexGenEyeS[0], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenT[0] != 0) tc.y = genOne(1, uTexGenT[0], uTexGenObjT[0], uTexGenEyeT[0], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenR[0] != 0) tc.z = genOne(2, uTexGenR[0], uTexGenObjR[0], uTexGenEyeR[0], objPos, eyePos, Nn_l, viewDir);\n"
"  vTexCoord[0] = uTexMatrix[0] * tc;\n"
"  tc = aTexCoord1;\n"
"  if (uTexGenS[1] != 0) tc.x = genOne(0, uTexGenS[1], uTexGenObjS[1], uTexGenEyeS[1], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenT[1] != 0) tc.y = genOne(1, uTexGenT[1], uTexGenObjT[1], uTexGenEyeT[1], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenR[1] != 0) tc.z = genOne(2, uTexGenR[1], uTexGenObjR[1], uTexGenEyeR[1], objPos, eyePos, Nn_l, viewDir);\n"
"  vTexCoord[1] = uTexMatrix[1] * tc;\n"
"  tc = aTexCoord2;\n"
"  if (uTexGenS[2] != 0) tc.x = genOne(0, uTexGenS[2], uTexGenObjS[2], uTexGenEyeS[2], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenT[2] != 0) tc.y = genOne(1, uTexGenT[2], uTexGenObjT[2], uTexGenEyeT[2], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenR[2] != 0) tc.z = genOne(2, uTexGenR[2], uTexGenObjR[2], uTexGenEyeR[2], objPos, eyePos, Nn_l, viewDir);\n"
"  vTexCoord[2] = uTexMatrix[2] * tc;\n"
"  tc = aTexCoord3;\n"
"  if (uTexGenS[3] != 0) tc.x = genOne(0, uTexGenS[3], uTexGenObjS[3], uTexGenEyeS[3], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenT[3] != 0) tc.y = genOne(1, uTexGenT[3], uTexGenObjT[3], uTexGenEyeT[3], objPos, eyePos, Nn_l, viewDir);\n"
"  if (uTexGenR[3] != 0) tc.z = genOne(2, uTexGenR[3], uTexGenObjR[3], uTexGenEyeR[3], objPos, eyePos, Nn_l, viewDir);\n"
"  vTexCoord[3] = uTexMatrix[3] * tc;\n"
"  vFogFragCoord = uFogCoordEnabled ? aFogCoord : -eyePos.z;\n"
"  vClipDist[0] = (0 < uNumClipPlanes) ? dot(uClipPlanes[0], eyePos) : 1.0;\n"
"  vClipDist[1] = (1 < uNumClipPlanes) ? dot(uClipPlanes[1], eyePos) : 1.0;\n"
"  vClipDist[2] = (2 < uNumClipPlanes) ? dot(uClipPlanes[2], eyePos) : 1.0;\n"
"  vClipDist[3] = (3 < uNumClipPlanes) ? dot(uClipPlanes[3], eyePos) : 1.0;\n"
"  vClipDist[4] = (4 < uNumClipPlanes) ? dot(uClipPlanes[4], eyePos) : 1.0;\n"
"  vClipDist[5] = (5 < uNumClipPlanes) ? dot(uClipPlanes[5], eyePos) : 1.0;\n"
"  gl_Position = uProjection * eyePos;\n"
"}\n";

static const char* FFP_FRAGMENT_SHADER_FIXED =
"#version 300 es\n"
"precision highp float;\n"
"in vec4 vColor;\n"
"in vec4 vSecondary;\n"
"in float vFogFragCoord;\n"
"in vec4 vTexCoord[4];\n"
"in float vClipDist[6];\n"
"uniform bool uTexEnabled[4];\n"
"uniform int uTexTarget[4];\n"
"uniform sampler2D uTex2D0;\n"
"uniform sampler2D uTex2D1;\n"
"uniform sampler2D uTex2D2;\n"
"uniform sampler2D uTex2D3;\n"
"uniform samplerCube uTexCube0;\n"
"uniform samplerCube uTexCube1;\n"
"uniform samplerCube uTexCube2;\n"
"uniform samplerCube uTexCube3;\n"
"uniform int uEnvMode[4];\n"
"uniform vec4 uEnvColor[4];\n"
"uniform bool uAlphaTest;\n"
"uniform int uAlphaFunc;\n"
"uniform float uAlphaRef;\n"
"uniform bool uFogEnabled;\n"
"uniform int uFogMode;\n"
"uniform float uFogStart;\n"
"uniform float uFogEnd;\n"
"uniform float uFogDensity;\n"
"uniform vec4 uFogColor;\n"
"uniform int uNumClipPlanes;\n"
"out vec4 fragColor;\n"
"vec4 sampleTex0(vec4 tc) { if (uTexTarget[0] == 1) return texture(uTexCube0, tc.xyz); return texture(uTex2D0, tc.xy / tc.w); }\n"
"vec4 sampleTex1(vec4 tc) { if (uTexTarget[1] == 1) return texture(uTexCube1, tc.xyz); return texture(uTex2D1, tc.xy / tc.w); }\n"
"vec4 sampleTex2(vec4 tc) { if (uTexTarget[2] == 1) return texture(uTexCube2, tc.xyz); return texture(uTex2D2, tc.xy / tc.w); }\n"
"vec4 sampleTex3(vec4 tc) { if (uTexTarget[3] == 1) return texture(uTexCube3, tc.xyz); return texture(uTex2D3, tc.xy / tc.w); }\n"
"vec4 applyEnv(vec4 c, vec4 t, int mode, vec4 envColor) {\n"
"  if (mode == 0x2100) return c * t;\n"
"  else if (mode == 0x2101) return vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"
"  else if (mode == 0x1E01) return t;\n"
"  else if (mode == 0x0104) return vec4(c.rgb + t.rgb, c.a * t.a);\n"
"  else if (mode == 0x0BE2) return vec4(mix(c.rgb, envColor.rgb, t.rgb), c.a * t.a);\n"
"  return c * t;\n"
"}\n"
"void main() {\n"
"  vec4 color = vColor;\n"
"  color.rgb += vSecondary.rgb;\n"
"  if (uTexEnabled[0]) color = applyEnv(color, sampleTex0(vTexCoord[0]), uEnvMode[0], uEnvColor[0]);\n"
"  if (uTexEnabled[1]) color = applyEnv(color, sampleTex1(vTexCoord[1]), uEnvMode[1], uEnvColor[1]);\n"
"  if (uTexEnabled[2]) color = applyEnv(color, sampleTex2(vTexCoord[2]), uEnvMode[2], uEnvColor[2]);\n"
"  if (uTexEnabled[3]) color = applyEnv(color, sampleTex3(vTexCoord[3]), uEnvMode[3], uEnvColor[3]);\n"
"  if (uFogEnabled) {\n"
"    float d = abs(vFogFragCoord);\n"
"    float f;\n"
"    if (uFogMode == 0x2601) f = clamp((uFogEnd - d) / max(uFogEnd - uFogStart, 1e-6), 0.0, 1.0);\n"
"    else if (uFogMode == 0x0800) f = exp(-uFogDensity * d);\n"
"    else { float z = uFogDensity * d; f = exp(-z * z); }\n"
"    color.rgb = mix(uFogColor.rgb, color.rgb, f);\n"
"  }\n"
"  if (uAlphaTest) {\n"
"    float a = color.a;\n"
"    bool pass = true;\n"
"    if (uAlphaFunc == 0x0200) pass = false;\n"
"    else if (uAlphaFunc == 0x0201) pass = a < uAlphaRef;\n"
"    else if (uAlphaFunc == 0x0202) pass = a == uAlphaRef;\n"
"    else if (uAlphaFunc == 0x0203) pass = a <= uAlphaRef;\n"
"    else if (uAlphaFunc == 0x0204) pass = a > uAlphaRef;\n"
"    else if (uAlphaFunc == 0x0205) pass = a != uAlphaRef;\n"
"    else if (uAlphaFunc == 0x0206) pass = a >= uAlphaRef;\n"
"    if (!pass) discard;\n"
"  }\n"
"  if (uNumClipPlanes > 0) {\n"
"    if (vClipDist[0] < 0.0) discard;\n"
"    if (uNumClipPlanes > 1 && vClipDist[1] < 0.0) discard;\n"
"    if (uNumClipPlanes > 2 && vClipDist[2] < 0.0) discard;\n"
"    if (uNumClipPlanes > 3 && vClipDist[3] < 0.0) discard;\n"
"    if (uNumClipPlanes > 4 && vClipDist[4] < 0.0) discard;\n"
"    if (uNumClipPlanes > 5 && vClipDist[5] < 0.0) discard;\n"
"  }\n"
"  fragColor = color;\n"
"}\n";

/* ------------------------------------------------------------------------- */
/* Program creation                                                          */
/* ------------------------------------------------------------------------- */

static GLuint ffp_compile(GLenum type, const char* src) {
    GLuint shader = es3_functions.glCreateShader(type);
    if (!shader) return 0;
    const GLchar* s = src;
    es3_functions.glShaderSource(shader, 1, &s, NULL);
    es3_functions.glCompileShader(shader);
    GLint status = GL_FALSE;
    es3_functions.glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint len = 0;
        es3_functions.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            char* log = malloc(len);
            es3_functions.glGetShaderInfoLog(shader, len, NULL, log);
            printf("LTW-FFP: shader compile failed: %s\n", log);
            free(log);
        }
        es3_functions.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static void ffp_get_uniforms(ffp_context_t* c) {
    GLuint p = c->program;
    ffp_uniforms_t* l = &c->loc;
    char name[64];
    int i;
    l->modelView = es3_functions.glGetUniformLocation(p, "uModelView");
    l->projection = es3_functions.glGetUniformLocation(p, "uProjection");
    l->normalMatrix = es3_functions.glGetUniformLocation(p, "uNormalMatrix");
    l->normalize = es3_functions.glGetUniformLocation(p, "uNormalize");
    l->lighting = es3_functions.glGetUniformLocation(p, "uLighting");
    l->numLights = es3_functions.glGetUniformLocation(p, "uNumLights");
    l->lightEnabled = es3_functions.glGetUniformLocation(p, "uLightEnabled");
    l->lightSpot = es3_functions.glGetUniformLocation(p, "uLightSpot");
    l->lightPosition = es3_functions.glGetUniformLocation(p, "uLightPosition");
    l->lightAmbient = es3_functions.glGetUniformLocation(p, "uLightAmbient");
    l->lightDiffuse = es3_functions.glGetUniformLocation(p, "uLightDiffuse");
    l->lightSpecular = es3_functions.glGetUniformLocation(p, "uLightSpecular");
    l->lightSpotDir = es3_functions.glGetUniformLocation(p, "uLightSpotDir");
    l->lightSpotCos = es3_functions.glGetUniformLocation(p, "uLightSpotCos");
    l->lightSpotExp = es3_functions.glGetUniformLocation(p, "uLightSpotExp");
    l->lightAtten = es3_functions.glGetUniformLocation(p, "uLightAtten");
    l->matAmbient = es3_functions.glGetUniformLocation(p, "uMatAmbient");
    l->matDiffuse = es3_functions.glGetUniformLocation(p, "uMatDiffuse");
    l->matSpecular = es3_functions.glGetUniformLocation(p, "uMatSpecular");
    l->matEmission = es3_functions.glGetUniformLocation(p, "uMatEmission");
    l->matShininess = es3_functions.glGetUniformLocation(p, "uMatShininess");
    l->globalAmbient = es3_functions.glGetUniformLocation(p, "uGlobalAmbient");
    l->localViewer = es3_functions.glGetUniformLocation(p, "uLocalViewer");
    l->colorMaterial = es3_functions.glGetUniformLocation(p, "uColorMaterial");
    l->colorMaterialMode = es3_functions.glGetUniformLocation(p, "uColorMaterialMode");
    l->colorSum = es3_functions.glGetUniformLocation(p, "uColorSum");
    l->fogEnabled = es3_functions.glGetUniformLocation(p, "uFogEnabled");
    l->fogCoordEnabled = es3_functions.glGetUniformLocation(p, "uFogCoordEnabled");
    l->fogMode = es3_functions.glGetUniformLocation(p, "uFogMode");
    l->fogStart = es3_functions.glGetUniformLocation(p, "uFogStart");
    l->fogEnd = es3_functions.glGetUniformLocation(p, "uFogEnd");
    l->fogDensity = es3_functions.glGetUniformLocation(p, "uFogDensity");
    l->fogColor = es3_functions.glGetUniformLocation(p, "uFogColor");
    l->texEnabled = es3_functions.glGetUniformLocation(p, "uTexEnabled");
    l->texTarget = es3_functions.glGetUniformLocation(p, "uTexTarget");
    l->texMatrix = es3_functions.glGetUniformLocation(p, "uTexMatrix");
    l->texGenS = es3_functions.glGetUniformLocation(p, "uTexGenS");
    l->texGenT = es3_functions.glGetUniformLocation(p, "uTexGenT");
    l->texGenR = es3_functions.glGetUniformLocation(p, "uTexGenR");
    l->texGenObjS = es3_functions.glGetUniformLocation(p, "uTexGenObjS");
    l->texGenObjT = es3_functions.glGetUniformLocation(p, "uTexGenObjT");
    l->texGenObjR = es3_functions.glGetUniformLocation(p, "uTexGenObjR");
    l->texGenEyeS = es3_functions.glGetUniformLocation(p, "uTexGenEyeS");
    l->texGenEyeT = es3_functions.glGetUniformLocation(p, "uTexGenEyeT");
    l->texGenEyeR = es3_functions.glGetUniformLocation(p, "uTexGenEyeR");
    l->envMode = es3_functions.glGetUniformLocation(p, "uEnvMode");
    l->envColor = es3_functions.glGetUniformLocation(p, "uEnvColor");
    l->alphaTest = es3_functions.glGetUniformLocation(p, "uAlphaTest");
    l->alphaFunc = es3_functions.glGetUniformLocation(p, "uAlphaFunc");
    l->alphaRef = es3_functions.glGetUniformLocation(p, "uAlphaRef");
    l->numClipPlanes = es3_functions.glGetUniformLocation(p, "uNumClipPlanes");
    l->clipPlanes = es3_functions.glGetUniformLocation(p, "uClipPlanes");
    for (i = 0; i < FFP_MAX_TEX; i++) {
        snprintf(name, sizeof(name), "uTex2D%d", i);
        l->tex2D[i] = es3_functions.glGetUniformLocation(p, name);
        snprintf(name, sizeof(name), "uTexCube%d", i);
        l->texCube[i] = es3_functions.glGetUniformLocation(p, name);
    }
}

static void ffp_ensure_program(ffp_context_t* c) {
    if (c->program != 0 || c->program_failed) return;
    GLuint vs = ffp_compile(GL_VERTEX_SHADER, FFP_VERTEX_SHADER);
    GLuint fs = ffp_compile(GL_FRAGMENT_SHADER, FFP_FRAGMENT_SHADER_FIXED);
    if (!vs || !fs) {
        if (vs) es3_functions.glDeleteShader(vs);
        if (fs) es3_functions.glDeleteShader(fs);
        c->program_failed = true;
        printf("LTW-FFP: failed to build the fixed-function shader\n");
        return;
    }
    GLuint p = es3_functions.glCreateProgram();
    es3_functions.glAttachShader(p, vs);
    es3_functions.glAttachShader(p, fs);
    /* bind the fixed attribute locations */
    es3_functions.glBindAttribLocation(p, FFP_LOC_POSITION, "aPosition");
    es3_functions.glBindAttribLocation(p, FFP_LOC_NORMAL, "aNormal");
    es3_functions.glBindAttribLocation(p, FFP_LOC_COLOR, "aColor");
    es3_functions.glBindAttribLocation(p, FFP_LOC_SECONDARY, "aSecondaryColor");
    es3_functions.glBindAttribLocation(p, FFP_LOC_FOGCOORD, "aFogCoord");
    {
        char name[32];
        int i;
        for (i = 0; i < FFP_MAX_TEX; i++) {
            snprintf(name, sizeof(name), "aTexCoord%d", i);
            es3_functions.glBindAttribLocation(p, FFP_LOC_TEXCOORD0 + i, name);
        }
    }
    es3_functions.glLinkProgram(p);
    es3_functions.glDeleteShader(vs);
    es3_functions.glDeleteShader(fs);
    GLint status = GL_FALSE;
    es3_functions.glGetProgramiv(p, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLint len = 0;
        es3_functions.glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            char* log = malloc(len);
            es3_functions.glGetProgramInfoLog(p, len, NULL, log);
            printf("LTW-FFP: program link failed: %s\n", log);
            free(log);
        }
        es3_functions.glDeleteProgram(p);
        c->program_failed = true;
        return;
    }
    c->program = p;
    ffp_get_uniforms(c);
}

/* ------------------------------------------------------------------------- */
/* Context lifecycle                                                         */
/* ------------------------------------------------------------------------- */

void ffp_context_init(void* ctxptr) {
    context_t* ctx = (context_t*)ctxptr;
    if (!ctx) return;
    if (ctx->ffp != NULL) return;
    {
        ffp_context_t* c = calloc(1, sizeof(ffp_context_t));
        if (!c) return;
        state_default(&c->state);
        ctx->ffp = c;
    }
}

static ffp_context_t* ffp_ensure(void) {
    if (!current_context) return NULL;
    if (current_context->ffp == NULL) ffp_context_init(current_context);
    return (ffp_context_t*)current_context->ffp;
}

void ffp_context_free(void* ptr) {
    ffp_context_t* c = (ffp_context_t*)ptr;
    int i;
    if (!c) return;
    /* The EGL context is already gone at this point, so GPU objects (the
     * program and buffers) are released with it. Only free CPU memory here. */
    if (c->imm_verts) free(c->imm_verts);
    if (c->idx_scratch) free(c->idx_scratch);
    if (c->attrib_stack) free(c->attrib_stack);
    if (c->client_stack) free(c->client_stack);
    if (c->dlists) {
        for (i = 0; i < c->dlists_capacity; i++) free(c->dlists[i].cmds);
        free(c->dlists);
    }
    free(c);
}

static void ffp_init_env(void) {
    g_compat = env_istrue("LTW_COMPAT");
    if (g_compat) printf("LTW-FFP: compatibility (fixed-function) context enabled\n");
}

__attribute__((constructor)) void ffp_constructor(void) {
    ffp_init_env();
}

/* ------------------------------------------------------------------------- */
/* Display list recording                                                    */
/* ------------------------------------------------------------------------- */

static void ffp_record(ffp_context_t* c, int op, int i0, int i1, int i2, const float* f, int nf) {
    ffp_dlist_t* dl;
    ffp_cmd_t* cmd;
    if (!c || !c->recording || c->record_list < 0) return;
    if (c->record_list >= c->dlists_capacity) {
        int newcap = c->dlists_capacity ? c->dlists_capacity * 2 : 64;
        while (c->record_list >= newcap) newcap *= 2;
        ffp_dlist_t* nd = realloc(c->dlists, sizeof(ffp_dlist_t) * newcap);
        if (!nd) return;
        memset(nd + c->dlists_capacity, 0, sizeof(ffp_dlist_t) * (newcap - c->dlists_capacity));
        c->dlists = nd;
        c->dlists_capacity = newcap;
    }
    dl = &c->dlists[c->record_list];
    dl->used = true;
    if (dl->count >= dl->capacity) {
        int newcap = dl->capacity ? dl->capacity * 2 : 32;
        ffp_cmd_t* nc = realloc(dl->cmds, sizeof(ffp_cmd_t) * newcap);
        if (!nc) return;
        dl->cmds = nc;
        dl->capacity = newcap;
    }
    cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->op = op;
    cmd->i0 = i0;
    cmd->i1 = i1;
    cmd->i2 = i2;
    if (nf > 8) nf = 8;
    if (f) memcpy(cmd->f, f, sizeof(float) * nf);
}

static void ffp_replay(ffp_context_t* c, int list);

/* ------------------------------------------------------------------------- */
/* Attribute / vertex helpers                                                */
/* ------------------------------------------------------------------------- */

static void ffp_set_attrib_pointer(GLenum loc, const ffp_pointer_t* a, bool normalized, int defsize) {
    GLboolean norm = GL_FALSE;
    if (a->enabled && a->ptr != NULL) {
        if (normalized) {
            switch (a->type) {
                case GL_BYTE:
                case GL_UNSIGNED_BYTE:
                case GL_SHORT:
                case GL_UNSIGNED_SHORT:
                case GL_INT:
                case GL_UNSIGNED_INT:
                    norm = GL_TRUE;
                    break;
                default:
                    norm = GL_FALSE;
                    break;
            }
        }
        es3_functions.glVertexAttribPointer(loc, a->size ? a->size : defsize, a->type,
                                            norm, a->stride, a->ptr);
        es3_functions.glEnableVertexAttribArray(loc);
    } else {
        es3_functions.glDisableVertexAttribArray(loc);
    }
}

/* Apply the current fixed-function client arrays to the ES attribute state. */
static void ffp_apply_arrays(ffp_context_t* c) {
    ffp_state_t* s = &c->state;
    int u;
    FFP_MARK(c);
    es3_functions.glDisableVertexAttribArray(FFP_LOC_POSITION);
    ffp_set_attrib_pointer(FFP_LOC_POSITION, &s->vertex_array, false, 4);
    if (!s->vertex_array.enabled) es3_functions.glVertexAttrib4f(FFP_LOC_POSITION, 0.0f, 0.0f, 0.0f, 1.0f);
    ffp_set_attrib_pointer(FFP_LOC_NORMAL, &s->normal_array, true, 3);
    if (!s->normal_array.enabled)
        es3_functions.glVertexAttrib3f(FFP_LOC_NORMAL, s->current_normal[0], s->current_normal[1], s->current_normal[2]);
    ffp_set_attrib_pointer(FFP_LOC_COLOR, &s->color_array, true, 4);
    if (!s->color_array.enabled)
        es3_functions.glVertexAttrib4f(FFP_LOC_COLOR, s->current_color[0], s->current_color[1], s->current_color[2], s->current_color[3]);
    ffp_set_attrib_pointer(FFP_LOC_SECONDARY, &s->secondary_array, true, 4);
    if (!s->secondary_array.enabled)
        es3_functions.glVertexAttrib4f(FFP_LOC_SECONDARY, s->current_secondary[0], s->current_secondary[1], s->current_secondary[2], s->current_secondary[3]);
    ffp_set_attrib_pointer(FFP_LOC_FOGCOORD, &s->fog_array, false, 1);
    if (!s->fog_array.enabled) es3_functions.glVertexAttrib1f(FFP_LOC_FOGCOORD, s->current_fogcoord);
    for (u = 0; u < FFP_MAX_TEX; u++) {
        ffp_set_attrib_pointer(FFP_LOC_TEXCOORD0 + u, &s->texcoord_array[u], false, 4);
        if (!s->texcoord_array[u].enabled)
            es3_functions.glVertexAttrib4f(FFP_LOC_TEXCOORD0 + u,
                                           s->current_texcoord[u][0], s->current_texcoord[u][1],
                                           s->current_texcoord[u][2], s->current_texcoord[u][3]);
    }
}

/* ------------------------------------------------------------------------- */
/* Uniform upload                                                            */
/* ------------------------------------------------------------------------- */

static void ffp_upload_uniforms(ffp_context_t* c) {
    ffp_state_t* s = &c->state;
    ffp_uniforms_t* l = &c->loc;
    float normalMatrix[9];
    int enabled[FFP_MAX_LIGHTS];
    int spotlight[FFP_MAX_LIGHTS];
    int i;
    mat3_normal_matrix(s->modelview, normalMatrix);
    for (i = 0; i < FFP_MAX_LIGHTS; i++) {
        enabled[i] = s->light.enabled[i] ? 1 : 0;
        spotlight[i] = s->light.spotlight[i] ? 1 : 0;
    }
    es3_functions.glUniformMatrix4fv(l->modelView, 1, GL_FALSE, s->modelview);
    es3_functions.glUniformMatrix4fv(l->projection, 1, GL_FALSE, s->projection);
    es3_functions.glUniformMatrix3fv(l->normalMatrix, 1, GL_FALSE, normalMatrix);
    es3_functions.glUniform1i(l->normalize, s->normalize || s->rescale_normal);
    es3_functions.glUniform1i(l->lighting, s->lighting);
    es3_functions.glUniform1i(l->numLights, FFP_MAX_LIGHTS);
    es3_functions.glUniform1iv(l->lightEnabled, FFP_MAX_LIGHTS, enabled);
    es3_functions.glUniform1iv(l->lightSpot, FFP_MAX_LIGHTS, spotlight);
    es3_functions.glUniform4fv(l->lightPosition, FFP_MAX_LIGHTS, (float*)s->light.position);
    es3_functions.glUniform4fv(l->lightAmbient, FFP_MAX_LIGHTS, (float*)s->light.ambient);
    es3_functions.glUniform4fv(l->lightDiffuse, FFP_MAX_LIGHTS, (float*)s->light.diffuse);
    es3_functions.glUniform4fv(l->lightSpecular, FFP_MAX_LIGHTS, (float*)s->light.specular);
    es3_functions.glUniform3fv(l->lightSpotDir, FFP_MAX_LIGHTS, (float*)s->light.spot_dir);
    es3_functions.glUniform1fv(l->lightSpotCos, FFP_MAX_LIGHTS, s->light.spot_cos);
    es3_functions.glUniform1fv(l->lightSpotExp, FFP_MAX_LIGHTS, s->light.spot_exp);
    es3_functions.glUniform3fv(l->lightAtten, FFP_MAX_LIGHTS, (float*)s->light.atten);
    es3_functions.glUniform4fv(l->matAmbient, 1, s->material_front.ambient);
    es3_functions.glUniform4fv(l->matDiffuse, 1, s->material_front.diffuse);
    es3_functions.glUniform4fv(l->matSpecular, 1, s->material_front.specular);
    es3_functions.glUniform4fv(l->matEmission, 1, s->material_front.emission);
    es3_functions.glUniform1f(l->matShininess, s->material_front.shininess);
    es3_functions.glUniform4fv(l->globalAmbient, 1, s->light.model_ambient);
    es3_functions.glUniform1i(l->localViewer, s->light.local_viewer);
    es3_functions.glUniform1i(l->colorMaterial, s->color_material);
    es3_functions.glUniform1i(l->colorMaterialMode, s->color_material_mode);
    es3_functions.glUniform1i(l->colorSum, s->color_sum);
    es3_functions.glUniform1i(l->fogEnabled, s->fog_enabled);
    es3_functions.glUniform1i(l->fogCoordEnabled, s->fog_coord_src == GL_FOG_COORD);
    es3_functions.glUniform1i(l->fogMode, s->fog_mode);
    es3_functions.glUniform1f(l->fogStart, s->fog_start);
    es3_functions.glUniform1f(l->fogEnd, s->fog_end);
    es3_functions.glUniform1f(l->fogDensity, s->fog_density);
    es3_functions.glUniform4fv(l->fogColor, 1, s->fog_color);
    es3_functions.glUniform1iv(l->texEnabled, FFP_MAX_TEX, s->tex.enabled);
    es3_functions.glUniform1iv(l->texTarget, FFP_MAX_TEX, s->tex.target);
    es3_functions.glUniformMatrix4fv(l->texMatrix, FFP_MAX_TEX, GL_FALSE, (float*)s->tex.matrix);
    es3_functions.glUniform1iv(l->texGenS, FFP_MAX_TEX, s->tex.gen_mode[0]);
    es3_functions.glUniform1iv(l->texGenT, FFP_MAX_TEX, s->tex.gen_mode[1]);
    es3_functions.glUniform1iv(l->texGenR, FFP_MAX_TEX, s->tex.gen_mode[2]);
    es3_functions.glUniform4fv(l->texGenObjS, FFP_MAX_TEX, (float*)s->tex.gen_object[0]);
    es3_functions.glUniform4fv(l->texGenObjT, FFP_MAX_TEX, (float*)s->tex.gen_object[1]);
    es3_functions.glUniform4fv(l->texGenObjR, FFP_MAX_TEX, (float*)s->tex.gen_object[2]);
    es3_functions.glUniform4fv(l->texGenEyeS, FFP_MAX_TEX, (float*)s->tex.gen_eye[0]);
    es3_functions.glUniform4fv(l->texGenEyeT, FFP_MAX_TEX, (float*)s->tex.gen_eye[1]);
    es3_functions.glUniform4fv(l->texGenEyeR, FFP_MAX_TEX, (float*)s->tex.gen_eye[2]);
    es3_functions.glUniform1iv(l->envMode, FFP_MAX_TEX, s->tex.env_mode);
    es3_functions.glUniform4fv(l->envColor, FFP_MAX_TEX, (float*)s->tex.env_color);
    es3_functions.glUniform1i(l->alphaTest, s->alpha_test);
    es3_functions.glUniform1i(l->alphaFunc, s->alpha_func);
    es3_functions.glUniform1f(l->alphaRef, s->alpha_ref);
    {
        /* Upload clip planes in eye space. */
        float planes[FFP_MAX_CLIP][4];
        int count = 0;
        int p;
        for (p = 0; p < FFP_MAX_CLIP; p++) {
            if (!s->clip_enabled[p]) continue;
            planes[count][0] = s->clip_plane[p][0];
            planes[count][1] = s->clip_plane[p][1];
            planes[count][2] = s->clip_plane[p][2];
            planes[count][3] = s->clip_plane[p][3];
            count++;
        }
        es3_functions.glUniform1i(l->numClipPlanes, count);
        if (count > 0) es3_functions.glUniform4fv(l->clipPlanes, count, (float*)planes);
    }
    for (i = 0; i < FFP_MAX_TEX; i++) {
        if (l->tex2D[i] >= 0) es3_functions.glUniform1i(l->tex2D[i], i);
        if (l->texCube[i] >= 0) es3_functions.glUniform1i(l->texCube[i], i);
    }
}

/* ------------------------------------------------------------------------- */
/* Draw path                                                                 */
/* ------------------------------------------------------------------------- */

static GLuint ffp_read_index(GLenum type, const void* base, GLsizei i) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return ((const GLubyte*)base)[i];
        case GL_UNSIGNED_SHORT: return ((const GLushort*)base)[i];
        case GL_UNSIGNED_INT: return ((const GLuint*)base)[i];
        default: return 0;
    }
}

static bool ffp_mode_native(GLenum mode) {
    switch (mode) {
        case GL_POINTS:
        case GL_LINES:
        case GL_LINE_LOOP:
        case GL_LINE_STRIP:
        case GL_TRIANGLES:
        case GL_TRIANGLE_STRIP:
        case GL_TRIANGLE_FAN:
            return true;
        default:
            return false;
    }
}

static void ffp_ensure_idx(ffp_context_t* c, int n) {
    if (n <= c->idx_capacity) return;
    int newcap = c->idx_capacity ? c->idx_capacity * 2 : 256;
    while (newcap < n) newcap *= 2;
    GLuint* ni = realloc(c->idx_scratch, sizeof(GLuint) * newcap);
    if (!ni) return;
    c->idx_scratch = ni;
    c->idx_capacity = newcap;
}

/* Issue a draw, converting quads/polygons to triangles when required. */
static int ffp_type_size(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return 1;
        case GL_UNSIGNED_SHORT: return 2;
        case GL_UNSIGNED_INT: return 4;
        default: return 0;
    }
}

static void ffp_issue(GLenum mode, GLsizei count, bool indexed, GLenum itype, const void* iptr, GLint first) {
    ffp_context_t* c = FFP;
    bool mapped = false;
    if (!c) return;
    if (ffp_mode_native(mode)) {
        if (indexed) es3_functions.glDrawElements(mode, count, itype, iptr);
        else es3_functions.glDrawArrays(mode, first, count);
        return;
    }
    if (count < 3) return;
    ffp_ensure_idx(c, count * 3 + 6);
    if (!c->idx_scratch) return;
    /* If indices live in a buffer object, map them so we can read them on host. */
    if (indexed) {
        GLint elemBuf = 0;
        es3_functions.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elemBuf);
        if (elemBuf != 0) {
            int ts = ffp_type_size(itype);
            void* map = NULL;
            if (ts > 0)
                map = es3_functions.glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)iptr,
                                                     (GLsizeiptr)count * ts, GL_MAP_READ_BIT);
            if (map == NULL) return; /* cannot convert safely */
            iptr = map;
            mapped = true;
        }
    }
    {
        GLuint* out = c->idx_scratch;
        int n = 0;
        int i;
        GLint prevIbo = 0;
#define SRC(k) (indexed ? ffp_read_index(itype, iptr, (k)) : (GLuint)(first + (k)))
        if (mode == GL_QUADS) {
            for (i = 0; i + 3 < count; i += 4) {
                GLuint a = SRC(i), b = SRC(i + 1), cix = SRC(i + 2), d = SRC(i + 3);
                out[n++] = a; out[n++] = b; out[n++] = cix;
                out[n++] = a; out[n++] = cix; out[n++] = d;
            }
        } else if (mode == GL_QUAD_STRIP) {
            for (i = 0; i + 3 < count; i += 2) {
                GLuint a = SRC(i), b = SRC(i + 1), cix = SRC(i + 3), d = SRC(i + 2);
                out[n++] = a; out[n++] = b; out[n++] = cix;
                out[n++] = a; out[n++] = cix; out[n++] = d;
            }
        } else { /* GL_POLYGON: triangle fan */
            for (i = 1; i + 1 < count; i++) {
                out[n++] = SRC(0); out[n++] = SRC(i); out[n++] = SRC(i + 1);
            }
        }
#undef SRC
        if (mapped) {
            es3_functions.glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            mapped = false;
        }
        if (n == 0) return;
        if (c->quad_ibo == 0) es3_functions.glGenBuffers(1, &c->quad_ibo);
        es3_functions.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevIbo);
        es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->quad_ibo);
        es3_functions.glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GLuint) * n, out, GL_STREAM_DRAW);
        es3_functions.glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, 0);
        es3_functions.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prevIbo);
    }
}

static void ffp_begin_draw(ffp_context_t* c) {
    ffp_ensure_program(c);
    if (c->program == 0) return;
    es3_functions.glUseProgram(c->program);
    ffp_upload_uniforms(c);
    ffp_apply_arrays(c);
}

static void ffp_end_draw(void) {
    if (current_context) es3_functions.glUseProgram(current_context->program);
}

/* ------------------------------------------------------------------------- */
/* Immediate mode                                                            */
/* ------------------------------------------------------------------------- */

static void ffp_imm_reserve(ffp_context_t* c, int extra) {
    if (c->imm_count + extra <= c->imm_capacity) return;
    int newcap = c->imm_capacity ? c->imm_capacity * 2 : 256;
    while (newcap < c->imm_count + extra) newcap *= 2;
    ffp_vert_t* nv = realloc(c->imm_verts, sizeof(ffp_vert_t) * newcap);
    if (!nv) return;
    c->imm_verts = nv;
    c->imm_capacity = newcap;
}

static void ffp_imm_push(ffp_context_t* c, const float* pos) {
    ffp_state_t* s = &c->state;
    ffp_vert_t* v;
    ffp_imm_reserve(c, 1);
    if (!c->imm_verts) return;
    v = &c->imm_verts[c->imm_count++];
    v->position[0] = pos[0]; v->position[1] = pos[1]; v->position[2] = pos[2]; v->position[3] = pos[3];
    v->normal[0] = s->current_normal[0]; v->normal[1] = s->current_normal[1]; v->normal[2] = s->current_normal[2];
    memcpy(v->color, s->current_color, sizeof(float) * 4);
    memcpy(v->secondary, s->current_secondary, sizeof(float) * 4);
    v->fogcoord = s->current_fogcoord;
    memcpy(v->texcoord, s->current_texcoord, sizeof(float) * 4 * FFP_MAX_TEX);
}

/* Point the fixed attributes at the immediate vertex buffer. */
static void ffp_imm_bind(ffp_context_t* c) {
    size_t stride = sizeof(ffp_vert_t);
    GLint prevArray = current_context->bound_buffers[0];
    if (c->imm_vbo == 0) es3_functions.glGenBuffers(1, &c->imm_vbo);
    es3_functions.glBindBuffer(GL_ARRAY_BUFFER, c->imm_vbo);
    es3_functions.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(stride * c->imm_count), c->imm_verts, GL_STREAM_DRAW);
    es3_functions.glVertexAttribPointer(FFP_LOC_POSITION, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ffp_vert_t, position));
    es3_functions.glVertexAttribPointer(FFP_LOC_NORMAL, 3, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ffp_vert_t, normal));
    es3_functions.glVertexAttribPointer(FFP_LOC_COLOR, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ffp_vert_t, color));
    es3_functions.glVertexAttribPointer(FFP_LOC_SECONDARY, 4, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ffp_vert_t, secondary));
    es3_functions.glVertexAttribPointer(FFP_LOC_FOGCOORD, 1, GL_FLOAT, GL_FALSE, stride, (const void*)offsetof(ffp_vert_t, fogcoord));
    {
        int u;
        for (u = 0; u < FFP_MAX_TEX; u++) {
            es3_functions.glVertexAttribPointer(FFP_LOC_TEXCOORD0 + u, 4, GL_FLOAT, GL_FALSE, stride,
                                                (const void*)(offsetof(ffp_vert_t, texcoord) + sizeof(float) * 4 * u));
        }
    }
    es3_functions.glEnableVertexAttribArray(FFP_LOC_POSITION);
    es3_functions.glEnableVertexAttribArray(FFP_LOC_NORMAL);
    es3_functions.glEnableVertexAttribArray(FFP_LOC_COLOR);
    es3_functions.glEnableVertexAttribArray(FFP_LOC_SECONDARY);
    es3_functions.glEnableVertexAttribArray(FFP_LOC_FOGCOORD);
    {
        int u;
        for (u = 0; u < FFP_MAX_TEX; u++) es3_functions.glEnableVertexAttribArray(FFP_LOC_TEXCOORD0 + u);
    }
    /* Restore the application's array buffer binding (tracked internally). */
    es3_functions.glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArray);
}

static void ffp_imm_flush(ffp_context_t* c) {
    if (c->imm_count == 0) return;
    ffp_begin_draw(c);
    if (c->program != 0) {
        ffp_imm_bind(c);
        ffp_issue(c->state.begin_mode, c->imm_count, false, 0, NULL, 0);
        ffp_apply_arrays(c);
    }
    c->imm_count = 0;
    ffp_end_draw();
}

/* ------------------------------------------------------------------------- */
/* Enable / disable                                                          */
/* ------------------------------------------------------------------------- */

static bool ffp_is_ff_cap(GLenum cap) {
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FFP_MAX_LIGHTS) return true;
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + FFP_MAX_CLIP) return true;
    switch (cap) {
        case GL_LIGHTING:
        case GL_COLOR_MATERIAL:
        case GL_FOG:
        case GL_ALPHA_TEST:
        case GL_NORMALIZE:
        case GL_RESCALE_NORMAL:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_GEN_S:
        case GL_TEXTURE_GEN_T:
        case GL_TEXTURE_GEN_R:
        case GL_TEXTURE_GEN_Q:
        case GL_COLOR_SUM:
        case GL_LINE_SMOOTH:
        case GL_POLYGON_SMOOTH:
        case GL_POLYGON_STIPPLE:
            return true;
        default:
            return false;
    }
}

bool ffp_handle_enable(GLenum cap, GLboolean enable) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    if (!ffp_is_ff_cap(cap)) return false;
    if (!c) return true; /* swallow FF caps when no context: nothing to forward */
    s = &c->state;
    if (cap == GL_LIGHTING || cap == GL_COLOR_MATERIAL || cap == GL_FOG ||
        cap == GL_ALPHA_TEST || cap == GL_NORMALIZE || cap == GL_RESCALE_NORMAL ||
        cap == GL_COLOR_SUM ||
        (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FFP_MAX_LIGHTS) ||
        (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + FFP_MAX_CLIP) ||
        cap == GL_TEXTURE_GEN_S || cap == GL_TEXTURE_GEN_T ||
        cap == GL_TEXTURE_GEN_R || cap == GL_TEXTURE_GEN_Q)
        FFP_MARK(c);
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + FFP_MAX_LIGHTS) {
        s->light.enabled[cap - GL_LIGHT0] = enable ? 1 : 0;
    } else if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + FFP_MAX_CLIP) {
        s->clip_enabled[cap - GL_CLIP_PLANE0] = enable ? 1 : 0;
    } else {
        switch (cap) {
            case GL_LIGHTING: s->lighting = enable; break;
            case GL_COLOR_MATERIAL: s->color_material = enable; break;
            case GL_FOG: s->fog_enabled = enable; break;
            case GL_ALPHA_TEST: s->alpha_test = enable; break;
            case GL_NORMALIZE: s->normalize = enable; break;
            case GL_RESCALE_NORMAL: s->rescale_normal = enable; break;
            case GL_COLOR_SUM: s->color_sum = enable; break;
            case GL_TEXTURE_2D: {
                int u = s->active_texture - GL_TEXTURE0;
                if (u >= 0 && u < FFP_MAX_TEX) { s->tex.enabled[u] = enable; s->tex.target[u] = 0; }
                break;
            }
            case GL_TEXTURE_CUBE_MAP: {
                int u = s->active_texture - GL_TEXTURE0;
                if (u >= 0 && u < FFP_MAX_TEX) { s->tex.enabled[u] = enable; s->tex.target[u] = enable ? 1 : 0; }
                break;
            }
            case GL_TEXTURE_GEN_S:
            case GL_TEXTURE_GEN_T:
            case GL_TEXTURE_GEN_R:
            case GL_TEXTURE_GEN_Q: {
                int u = s->active_texture - GL_TEXTURE0;
                int comp = (cap == GL_TEXTURE_GEN_S) ? 0 : (cap == GL_TEXTURE_GEN_T) ? 1 : (cap == GL_TEXTURE_GEN_R) ? 2 : 3;
                if (u >= 0 && u < FFP_MAX_TEX) s->tex.gen_mode[comp][u] = enable ? 1 : 0;
                break;
            }
            case GL_LINE_SMOOTH: s->line_smooth = enable; break;
            case GL_POLYGON_SMOOTH: s->polygon_smooth = enable; break;
            case GL_POLYGON_STIPPLE: s->polygon_stipple = enable; break;
            default: break;
        }
    }
    if (c->recording) {
        float f[1] = { (float)enable };
        ffp_record(c, 1, (int)cap, 0, 0, f, 1);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Getters                                                                   */
/* ------------------------------------------------------------------------- */

bool ffp_handle_getintegerv(GLenum pname, GLint* data) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    if (!c) return false;
    s = &c->state;
    switch (pname) {
        case GL_MATRIX_MODE: *data = s->matrix_mode; return true;
        case GL_MODELVIEW_STACK_DEPTH: *data = s->modelview_top + 1; return true;
        case GL_PROJECTION_STACK_DEPTH: *data = s->projection_top + 1; return true;
        case GL_TEXTURE_STACK_DEPTH: {
            int u = s->active_texture - GL_TEXTURE0;
            if (u < 0 || u >= FFP_MAX_TEX) u = 0;
            *data = s->texture_top[u] + 1;
            return true;
        }
        case GL_MAX_TEXTURE_UNITS: *data = FFP_MAX_TEX; return true;
        case GL_MAX_LIGHTS: *data = FFP_MAX_LIGHTS; return true;
        case GL_MAX_CLIP_PLANES: *data = FFP_MAX_CLIP; return true;
        case GL_MAX_MODELVIEW_STACK_DEPTH: *data = FFP_MAX_MATRIX_STACK; return true;
        case GL_MAX_PROJECTION_STACK_DEPTH: *data = FFP_MAX_MATRIX_STACK; return true;
        case GL_MAX_TEXTURE_STACK_DEPTH: *data = FFP_MAX_MATRIX_STACK; return true;
        case GL_CURRENT_COLOR:
            data[0] = (GLint)(s->current_color[0] * 2147483647.0f);
            data[1] = (GLint)(s->current_color[1] * 2147483647.0f);
            data[2] = (GLint)(s->current_color[2] * 2147483647.0f);
            data[3] = (GLint)(s->current_color[3] * 2147483647.0f);
            return true;
        case GL_LIGHTING: *data = s->lighting; return true;
        case GL_FOG: *data = s->fog_enabled; return true;
        case GL_ALPHA_TEST: *data = s->alpha_test; return true;
        case GL_SHADE_MODEL: *data = s->shade_model; return true;
        case GL_COLOR_MATERIAL: *data = s->color_material; return true;
        case GL_NORMALIZE: *data = s->normalize; return true;
        case GL_ALPHA_TEST_FUNC: *data = s->alpha_func; return true;
        case GL_ALPHA_TEST_REF: *data = (GLint)(s->alpha_ref * 2147483647.0f); return true;
        case GL_FOG_MODE: *data = s->fog_mode; return true;
        case GL_ACTIVE_TEXTURE: *data = s->active_texture; return true;
        case GL_CLIENT_ACTIVE_TEXTURE: *data = s->client_active_texture; return true;
        case GL_TEXTURE_BINDING_2D:
        case GL_TEXTURE_BINDING_CUBE_MAP:
            return false; /* let the driver answer */
        default:
            break;
    }
    if (pname >= GL_LIGHT0 && pname < GL_LIGHT0 + FFP_MAX_LIGHTS) {
        *data = s->light.enabled[pname - GL_LIGHT0]; return true;
    }
    if (pname >= GL_CLIP_PLANE0 && pname < GL_CLIP_PLANE0 + FFP_MAX_CLIP) {
        *data = s->clip_enabled[pname - GL_CLIP_PLANE0]; return true;
    }
    return false;
}

bool ffp_handle_getfloatv(GLenum pname, GLfloat* data) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    if (!c) return false;
    s = &c->state;
    switch (pname) {
        case GL_CURRENT_COLOR: memcpy(data, s->current_color, sizeof(float) * 4); return true;
        case GL_CURRENT_NORMAL: memcpy(data, s->current_normal, sizeof(float) * 3); return true;
        case GL_CURRENT_TEXTURE_COORDS: {
            int u = s->active_texture - GL_TEXTURE0;
            if (u < 0 || u >= FFP_MAX_TEX) u = 0;
            memcpy(data, s->current_texcoord[u], sizeof(float) * 4);
            return true;
        }
        case GL_MODELVIEW_MATRIX: memcpy(data, s->modelview, sizeof(float) * 16); return true;
        case GL_PROJECTION_MATRIX: memcpy(data, s->projection, sizeof(float) * 16); return true;
        case GL_TEXTURE_MATRIX: {
            int u = s->active_texture - GL_TEXTURE0;
            if (u < 0 || u >= FFP_MAX_TEX) u = 0;
            memcpy(data, s->texture[u], sizeof(float) * 16);
            return true;
        }
        case GL_LIGHT_MODEL_AMBIENT: memcpy(data, s->light.model_ambient, sizeof(float) * 4); return true;
        case GL_FOG_COLOR: memcpy(data, s->fog_color, sizeof(float) * 4); return true;
        case GL_FOG_START: *data = s->fog_start; return true;
        case GL_FOG_END: *data = s->fog_end; return true;
        case GL_FOG_DENSITY: *data = s->fog_density; return true;
        case GL_ALPHA_TEST_REF: *data = s->alpha_ref; return true;
        default: break;
    }
    if (pname >= GL_LIGHT0 && pname < GL_LIGHT0 + FFP_MAX_LIGHTS) {
        int i = pname - GL_LIGHT0;
        data[0] = s->light.enabled[i] ? 1.0f : 0.0f;
        return true;
    }
    return false;
}

bool ffp_handle_getdoublev(GLenum pname, GLdouble* data) {
    GLfloat tmp[16];
    if (!ffp_handle_getfloatv(pname, tmp)) return false;
    {
        int n = 1;
        switch (pname) {
            case GL_CURRENT_COLOR:
            case GL_CURRENT_TEXTURE_COORDS:
            case GL_LIGHT_MODEL_AMBIENT:
            case GL_FOG_COLOR:
                n = 4; break;
            case GL_CURRENT_NORMAL: n = 3; break;
            case GL_MODELVIEW_MATRIX:
            case GL_PROJECTION_MATRIX:
            case GL_TEXTURE_MATRIX:
                n = 16; break;
            default: n = 1; break;
        }
        for (int i = 0; i < n; i++) data[i] = (GLdouble)tmp[i];
    }
    return true;
}

static int ffp_query_count(GLenum pname) {
    switch (pname) {
        case GL_CURRENT_COLOR:
        case GL_CURRENT_TEXTURE_COORDS:
        case GL_LIGHT_MODEL_AMBIENT:
        case GL_FOG_COLOR:
        case GL_VIEWPORT:
        case GL_SCISSOR_BOX:
        case GL_COLOR_CLEAR_VALUE:
        case GL_DEPTH_RANGE:
            return 4;
        case GL_CURRENT_NORMAL:
        case GL_LINE_WIDTH_RANGE:
            return 3;
        case GL_MODELVIEW_MATRIX:
        case GL_PROJECTION_MATRIX:
        case GL_TEXTURE_MATRIX:
        case GL_MODELVIEW_STACK_DEPTH:
            return 16;
        default:
            return 1;
    }
}

void glGetFloatv(GLenum pname, GLfloat* data) {
    if (!current_context) return;
    if (ffp_handle_getfloatv(pname, data)) return;
    es3_functions.glGetFloatv(pname, data);
}

void glGetDoublev(GLenum pname, GLdouble* data) {
    int n, i;
    GLfloat tmp[16];
    if (!current_context) return;
    if (ffp_handle_getdoublev(pname, data)) return;
    n = ffp_query_count(pname);
    if (n > 16) n = 16;
    es3_functions.glGetFloatv(pname, tmp);
    for (i = 0; i < n; i++) data[i] = (GLdouble)tmp[i];
}

void glGetBooleanv(GLenum pname, GLboolean* data) {
    int n, i;
    GLint tmp[16];
    if (!current_context) return;
    if (ffp_handle_getintegerv(pname, tmp)) {
        n = ffp_query_count(pname);
        if (n > 16) n = 16;
        for (i = 0; i < n; i++) data[i] = tmp[i] ? GL_TRUE : GL_FALSE;
        return;
    }
    es3_functions.glGetBooleanv(pname, data);
}


static float* ffp_current_matrix(ffp_context_t* c) {
    ffp_state_t* s = &c->state;
    FFP_MARK(c);
    if (s->matrix_mode == GL_PROJECTION) return s->projection;
    if (s->matrix_mode == GL_TEXTURE) {
        int u = s->active_texture - GL_TEXTURE0;
        if (u < 0 || u >= FFP_MAX_TEX) u = 0;
        return s->texture[u];
    }
    return s->modelview;
}

static void ffp_record_mat(ffp_context_t* c, int op, const float* f, int nf) {
    if (c->recording) ffp_record(c, op, 0, 0, 0, f, nf);
}

void glMatrixMode(GLenum mode) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 10, (int)mode, 0, 0, NULL, 0); return; }
    c->state.matrix_mode = mode;
}

void glLoadIdentity(void) {
    ffp_context_t* c = FFP;
    if (!c) return;
    if (c->recording) { ffp_record(c, 11, 0, 0, 0, NULL, 0); return; }
    mat4_identity(ffp_current_matrix(c));
}

void glLoadMatrixf(const GLfloat* m) {
    ffp_context_t* c = FFP;
    if (!c) return;
    if (c->recording) { ffp_record(c, 12, 0, 0, 0, m, 16); return; }
    mat4_copy(ffp_current_matrix(c), m);
}

void glLoadMatrixd(const GLdouble* m) {
    float f[16];
    int i;
    for (i = 0; i < 16; i++) f[i] = (float)m[i];
    glLoadMatrixf(f);
}

void glMultMatrixf(const GLfloat* m) {
    ffp_context_t* c = FFP;
    if (!c) return;
    if (c->recording) { ffp_record(c, 13, 0, 0, 0, m, 16); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glMultMatrixd(const GLdouble* m) {
    float f[16];
    int i;
    for (i = 0; i < 16; i++) f[i] = (float)m[i];
    glMultMatrixf(f);
}

void glLoadTransposeMatrixf(const GLfloat* m) {
    float t[16];
    mat4_transpose(t, m);
    glLoadMatrixf(t);
}

void glLoadTransposeMatrixd(const GLdouble* m) {
    float f[16], t[16];
    int i;
    for (i = 0; i < 16; i++) f[i] = (float)m[i];
    mat4_transpose(t, f);
    glLoadMatrixf(t);
}

void glMultTransposeMatrixf(const GLfloat* m) {
    float t[16];
    mat4_transpose(t, m);
    glMultMatrixf(t);
}

void glMultTransposeMatrixd(const GLdouble* m) {
    float f[16], t[16];
    int i;
    for (i = 0; i < 16; i++) f[i] = (float)m[i];
    mat4_transpose(t, f);
    glMultMatrixf(t);
}

void glPushMatrix(void) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 14, 0, 0, 0, NULL, 0); return; }
    s = &c->state;
    if (s->matrix_mode == GL_PROJECTION) {
        if (s->projection_top + 1 < FFP_MAX_MATRIX_STACK) {
            mat4_copy(s->projection_stack[s->projection_top], s->projection);
            s->projection_top++;
        }
    } else if (s->matrix_mode == GL_TEXTURE) {
        int u = s->active_texture - GL_TEXTURE0;
        if (u < 0 || u >= FFP_MAX_TEX) u = 0;
        if (s->texture_top[u] + 1 < FFP_MAX_MATRIX_STACK) {
            mat4_copy(s->texture_stack[u][s->texture_top[u]], s->texture[u]);
            s->texture_top[u]++;
        }
    } else {
        if (s->modelview_top + 1 < FFP_MAX_MATRIX_STACK) {
            mat4_copy(s->modelview_stack[s->modelview_top], s->modelview);
            s->modelview_top++;
        }
    }
}

void glPopMatrix(void) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 15, 0, 0, 0, NULL, 0); return; }
    s = &c->state;
    if (s->matrix_mode == GL_PROJECTION) {
        if (s->projection_top > 0) { s->projection_top--; mat4_copy(s->projection, s->projection_stack[s->projection_top]); }
    } else if (s->matrix_mode == GL_TEXTURE) {
        int u = s->active_texture - GL_TEXTURE0;
        if (u < 0 || u >= FFP_MAX_TEX) u = 0;
        if (s->texture_top[u] > 0) { s->texture_top[u]--; mat4_copy(s->texture[u], s->texture_stack[u][s->texture_top[u]]); }
    } else {
        if (s->modelview_top > 0) { s->modelview_top--; mat4_copy(s->modelview, s->modelview_stack[s->modelview_top]); }
    }
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    ffp_context_t* c = FFP;
    float m[16];
    if (!c) return;
    mat4_ortho(m, l, r, b, t, n, f);
    if (c->recording) { ffp_record(c, 16, 0, 0, 0, m, 16); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    ffp_context_t* c = FFP;
    float m[16];
    if (!c) return;
    mat4_frustum(m, l, r, b, t, n, f);
    if (c->recording) { ffp_record(c, 17, 0, 0, 0, m, 16); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    ffp_context_t* c = FFP;
    float m[16];
    if (!c) return;
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
    if (c->recording) { float f[3] = { x, y, z }; ffp_record(c, 18, 0, 0, 0, f, 3); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glTranslated(GLdouble x, GLdouble y, GLdouble z) {
    glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    ffp_context_t* c = FFP;
    float m[16];
    if (!c) return;
    mat4_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
    if (c->recording) { float f[3] = { x, y, z }; ffp_record(c, 19, 0, 0, 0, f, 3); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glScaled(GLdouble x, GLdouble y, GLdouble z) {
    glScalef((GLfloat)x, (GLfloat)y, (GLfloat)z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    ffp_context_t* c = FFP;
    float m[16];
    float rad = angle * (FFP_PI / 180.0f);
    float co = cosf(rad), si = sinf(rad);
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-8f) return;
    x /= len; y /= len; z /= len;
    mat4_identity(m);
    m[0] = co + x * x * (1 - co);
    m[1] = y * x * (1 - co) + z * si;
    m[2] = z * x * (1 - co) - y * si;
    m[4] = x * y * (1 - co) - z * si;
    m[5] = co + y * y * (1 - co);
    m[6] = z * y * (1 - co) + x * si;
    m[8] = x * z * (1 - co) + y * si;
    m[9] = y * z * (1 - co) - x * si;
    m[10] = co + z * z * (1 - co);
    if (!c) return;
    if (c->recording) { float f[4] = { angle, x, y, z }; ffp_record(c, 20, 0, 0, 0, f, 4); return; }
    mat4_apply(ffp_current_matrix(c), m);
}

void glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) {
    glRotatef((GLfloat)angle, (GLfloat)x, (GLfloat)y, (GLfloat)z);
}

/* ------------------------------------------------------------------------- */
/* Immediate mode entry points                                               */
/* ------------------------------------------------------------------------- */

void glBegin(GLenum mode) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 30, (int)mode, 0, 0, NULL, 0); }
    c->state.in_begin_marker = 1;
    c->state.begin_mode = mode;
    c->imm_count = 0;
}

void glEnd(void) {
    ffp_context_t* c = FFP;
    if (!c) return;
    if (c->recording) { ffp_record(c, 31, 0, 0, 0, NULL, 0); }
    ffp_imm_flush(c);
    c->state.in_begin_marker = 0;
}

static void ffp_set_color4(float r, float g, float b, float a) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { float f[4] = { r, g, b, a }; ffp_record(c, 50, 0, 0, 0, f, 4); return; }
    c->state.current_color[0] = r; c->state.current_color[1] = g;
    c->state.current_color[2] = b; c->state.current_color[3] = a;
}
static void ffp_set_color3(float r, float g, float b) { ffp_set_color4(r, g, b, 1.0f); }

void glColor3f(GLfloat r, GLfloat g, GLfloat b) { ffp_set_color3(r, g, b); }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { ffp_set_color3((float)r, (float)g, (float)b); }
void glColor3b(GLbyte r, GLbyte g, GLbyte b) { ffp_set_color3(r / 127.0f, g / 127.0f, b / 127.0f); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { ffp_set_color3(r / 255.0f, g / 255.0f, b / 255.0f); }
void glColor3s(GLshort r, GLshort g, GLshort b) { ffp_set_color3(r / 32767.0f, g / 32767.0f, b / 32767.0f); }
void glColor3us(GLushort r, GLushort g, GLushort b) { ffp_set_color3(r / 65535.0f, g / 65535.0f, b / 65535.0f); }
void glColor3i(GLint r, GLint g, GLint b) { ffp_set_color3(r / 2147483647.0f, g / 2147483647.0f, b / 2147483647.0f); }
void glColor3ui(GLuint r, GLuint g, GLuint b) { ffp_set_color3(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ffp_set_color4(r, g, b, a); }
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { ffp_set_color4((float)r, (float)g, (float)b, (float)a); }
void glColor4b(GLbyte r, GLbyte g, GLbyte b, GLbyte a) { ffp_set_color4(r / 127.0f, g / 127.0f, b / 127.0f, a / 127.0f); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { ffp_set_color4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glColor4s(GLshort r, GLshort g, GLshort b, GLshort a) { ffp_set_color4(r / 32767.0f, g / 32767.0f, b / 32767.0f, a / 32767.0f); }
void glColor4us(GLushort r, GLushort g, GLushort b, GLushort a) { ffp_set_color4(r / 65535.0f, g / 65535.0f, b / 65535.0f, a / 65535.0f); }
void glColor4i(GLint r, GLint g, GLint b, GLint a) { ffp_set_color4(r / 2147483647.0f, g / 2147483647.0f, b / 2147483647.0f, a / 2147483647.0f); }
void glColor4ui(GLuint r, GLuint g, GLuint b, GLuint a) { ffp_set_color4(r / 4294967295.0f, g / 4294967295.0f, b / 4294967295.0f, a / 4294967295.0f); }

void glColor3fv(const GLfloat* v) { ffp_set_color3(v[0], v[1], v[2]); }
void glColor3dv(const GLdouble* v) { ffp_set_color3((float)v[0], (float)v[1], (float)v[2]); }
void glColor3bv(const GLbyte* v) { glColor3b(v[0], v[1], v[2]); }
void glColor3ubv(const GLubyte* v) { glColor3ub(v[0], v[1], v[2]); }
void glColor3sv(const GLshort* v) { glColor3s(v[0], v[1], v[2]); }
void glColor3usv(const GLushort* v) { glColor3us(v[0], v[1], v[2]); }
void glColor3iv(const GLint* v) { glColor3i(v[0], v[1], v[2]); }
void glColor3uiv(const GLuint* v) { glColor3ui(v[0], v[1], v[2]); }
void glColor4fv(const GLfloat* v) { ffp_set_color4(v[0], v[1], v[2], v[3]); }
void glColor4dv(const GLdouble* v) { ffp_set_color4((float)v[0], (float)v[1], (float)v[2], (float)v[3]); }
void glColor4bv(const GLbyte* v) { glColor4b(v[0], v[1], v[2], v[3]); }
void glColor4ubv(const GLubyte* v) { glColor4ub(v[0], v[1], v[2], v[3]); }
void glColor4sv(const GLshort* v) { glColor4s(v[0], v[1], v[2], v[3]); }
void glColor4usv(const GLushort* v) { glColor4us(v[0], v[1], v[2], v[3]); }
void glColor4iv(const GLint* v) { glColor4i(v[0], v[1], v[2], v[3]); }
void glColor4uiv(const GLuint* v) { glColor4ui(v[0], v[1], v[2], v[3]); }

static void ffp_set_normal3(float x, float y, float z) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { float f[3] = { x, y, z }; ffp_record(c, 51, 0, 0, 0, f, 3); return; }
    c->state.current_normal[0] = x; c->state.current_normal[1] = y; c->state.current_normal[2] = z;
}
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { ffp_set_normal3(x, y, z); }
void glNormal3d(GLdouble x, GLdouble y, GLdouble z) { ffp_set_normal3((float)x, (float)y, (float)z); }
void glNormal3b(GLbyte x, GLbyte y, GLbyte z) { ffp_set_normal3(x / 127.0f, y / 127.0f, z / 127.0f); }
void glNormal3s(GLshort x, GLshort y, GLshort z) { ffp_set_normal3(x / 32767.0f, y / 32767.0f, z / 32767.0f); }
void glNormal3i(GLint x, GLint y, GLint z) { ffp_set_normal3((float)x, (float)y, (float)z); }
void glNormal3fv(const GLfloat* v) { ffp_set_normal3(v[0], v[1], v[2]); }
void glNormal3dv(const GLdouble* v) { ffp_set_normal3((float)v[0], (float)v[1], (float)v[2]); }
void glNormal3bv(const GLbyte* v) { glNormal3b(v[0], v[1], v[2]); }
void glNormal3sv(const GLshort* v) { glNormal3s(v[0], v[1], v[2]); }
void glNormal3iv(const GLint* v) { glNormal3i(v[0], v[1], v[2]); }

static void ffp_set_texcoord(int unit, float x, float y, float z, float w) {
    ffp_context_t* c = FFP;
    if (!c || unit < 0 || unit >= FFP_MAX_TEX) return;
    FFP_MARK(c);
    if (c->recording) { float f[4] = { x, y, z, w }; ffp_record(c, 52, unit, 0, 0, f, 4); return; }
    c->state.current_texcoord[unit][0] = x;
    c->state.current_texcoord[unit][1] = y;
    c->state.current_texcoord[unit][2] = z;
    c->state.current_texcoord[unit][3] = w;
}
void glTexCoord1f(GLfloat x) { ffp_context_t* c = FFP; if (c) ffp_set_texcoord(c->state.active_texture - GL_TEXTURE0, x, 0, 0, 1); }
void glTexCoord2f(GLfloat x, GLfloat y) { ffp_context_t* c = FFP; if (c) ffp_set_texcoord(c->state.active_texture - GL_TEXTURE0, x, y, 0, 1); }
void glTexCoord3f(GLfloat x, GLfloat y, GLfloat z) { ffp_context_t* c = FFP; if (c) ffp_set_texcoord(c->state.active_texture - GL_TEXTURE0, x, y, z, 1); }
void glTexCoord4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { ffp_context_t* c = FFP; if (c) ffp_set_texcoord(c->state.active_texture - GL_TEXTURE0, x, y, z, w); }
void glTexCoord1d(GLdouble x) { glTexCoord1f((float)x); }
void glTexCoord2d(GLdouble x, GLdouble y) { glTexCoord2f((float)x, (float)y); }
void glTexCoord3d(GLdouble x, GLdouble y, GLdouble z) { glTexCoord3f((float)x, (float)y, (float)z); }
void glTexCoord4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { glTexCoord4f((float)x, (float)y, (float)z, (float)w); }
void glTexCoord1fv(const GLfloat* v) { glTexCoord1f(v[0]); }
void glTexCoord2fv(const GLfloat* v) { glTexCoord2f(v[0], v[1]); }
void glTexCoord3fv(const GLfloat* v) { glTexCoord3f(v[0], v[1], v[2]); }
void glTexCoord4fv(const GLfloat* v) { glTexCoord4f(v[0], v[1], v[2], v[3]); }

void glMultiTexCoord1f(GLenum target, GLfloat x) { ffp_set_texcoord((int)target - GL_TEXTURE0, x, 0, 0, 1); }
void glMultiTexCoord2f(GLenum target, GLfloat x, GLfloat y) { ffp_set_texcoord((int)target - GL_TEXTURE0, x, y, 0, 1); }
void glMultiTexCoord3f(GLenum target, GLfloat x, GLfloat y, GLfloat z) { ffp_set_texcoord((int)target - GL_TEXTURE0, x, y, z, 1); }
void glMultiTexCoord4f(GLenum target, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { ffp_set_texcoord((int)target - GL_TEXTURE0, x, y, z, w); }
void glMultiTexCoord1fv(GLenum target, const GLfloat* v) { glMultiTexCoord1f(target, v[0]); }
void glMultiTexCoord2fv(GLenum target, const GLfloat* v) { glMultiTexCoord2f(target, v[0], v[1]); }
void glMultiTexCoord3fv(GLenum target, const GLfloat* v) { glMultiTexCoord3f(target, v[0], v[1], v[2]); }
void glMultiTexCoord4fv(GLenum target, const GLfloat* v) { glMultiTexCoord4f(target, v[0], v[1], v[2], v[3]); }
void glMultiTexCoord2d(GLenum target, GLdouble x, GLdouble y) { glMultiTexCoord2f(target, (float)x, (float)y); }
void glMultiTexCoord3d(GLenum target, GLdouble x, GLdouble y, GLdouble z) { glMultiTexCoord3f(target, (float)x, (float)y, (float)z); }
void glMultiTexCoord4d(GLenum target, GLdouble x, GLdouble y, GLdouble z, GLdouble w) { glMultiTexCoord4f(target, (float)x, (float)y, (float)z, (float)w); }

void glSecondaryColor3f(GLfloat r, GLfloat g, GLfloat b) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { float f[3] = { r, g, b }; ffp_record(c, 53, 0, 0, 0, f, 3); return; }
    c->state.current_secondary[0] = r; c->state.current_secondary[1] = g; c->state.current_secondary[2] = b;
}
void glSecondaryColor3fv(const GLfloat* v) { glSecondaryColor3f(v[0], v[1], v[2]); }
void glSecondaryColor3ub(GLubyte r, GLubyte g, GLubyte b) { glSecondaryColor3f(r / 255.0f, g / 255.0f, b / 255.0f); }
void glSecondaryColor3ubv(const GLubyte* v) { glSecondaryColor3ub(v[0], v[1], v[2]); }

void glFogCoordf(GLfloat f) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { float ff[1] = { f }; ffp_record(c, 54, 0, 0, 0, ff, 1); return; }
    c->state.current_fogcoord = f;
}
void glFogCoordd(GLdouble f) { glFogCoordf((float)f); }
void glFogCoordfv(const GLfloat* v) { glFogCoordf(v[0]); }

void glEdgeFlag(GLboolean f) {
    ffp_context_t* c = FFP;
    if (c) c->state.current_edgeflag = f ? 1 : 0;
}
void glEdgeFlagv(const GLboolean* f) { glEdgeFlag(*f); }

/* Rectangles are just immediate-mode quads. */
static void ffp_rect(float x1, float y1, float x2, float y2) {
    glBegin(GL_QUADS);
    glVertex2f(x1, y1);
    glVertex2f(x2, y1);
    glVertex2f(x2, y2);
    glVertex2f(x1, y2);
    glEnd();
}
void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) { ffp_rect(x1, y1, x2, y2); }
void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { ffp_rect((float)x1, (float)y1, (float)x2, (float)y2); }
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { ffp_rect((float)x1, (float)y1, (float)x2, (float)y2); }
void glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) { ffp_rect(x1, y1, x2, y2); }
void glRectfv(const GLfloat* v1, const GLfloat* v2) { ffp_rect(v1[0], v1[1], v2[0], v2[1]); }
void glRectdv(const GLdouble* v1, const GLdouble* v2) { ffp_rect((float)v1[0], (float)v1[1], (float)v2[0], (float)v2[1]); }
void glRectiv(const GLint* v1, const GLint* v2) { ffp_rect((float)v1[0], (float)v1[1], (float)v2[0], (float)v2[1]); }
void glRectsv(const GLshort* v1, const GLshort* v2) { ffp_rect(v1[0], v1[1], v2[0], v2[1]); }

/* Vertex entry points. */
static void ffp_vertex4(float x, float y, float z, float w) {
    ffp_context_t* c = FFP;
    float p[4];
    if (!c) return;
    p[0] = x; p[1] = y; p[2] = z; p[3] = w;
    if (c->recording) { ffp_record(c, 40, 0, 0, 0, p, 4); return; }
    ffp_imm_push(c, p);
}
void glVertex2f(GLfloat x, GLfloat y) { ffp_vertex4(x, y, 0, 1); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ffp_vertex4(x, y, z, 1); }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { ffp_vertex4(x, y, z, w); }
void glVertex2d(GLdouble x, GLdouble y) { ffp_vertex4((float)x, (float)y, 0, 1); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { ffp_vertex4((float)x, (float)y, (float)z, 1); }
void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { ffp_vertex4((float)x, (float)y, (float)z, (float)w); }
void glVertex2i(GLint x, GLint y) { ffp_vertex4((float)x, (float)y, 0, 1); }
void glVertex3i(GLint x, GLint y, GLint z) { ffp_vertex4((float)x, (float)y, (float)z, 1); }
void glVertex4i(GLint x, GLint y, GLint z, GLint w) { ffp_vertex4((float)x, (float)y, (float)z, (float)w); }
void glVertex2s(GLshort x, GLshort y) { ffp_vertex4(x, y, 0, 1); }
void glVertex3s(GLshort x, GLshort y, GLshort z) { ffp_vertex4(x, y, z, 1); }
void glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) { ffp_vertex4(x, y, z, w); }
void glVertex2fv(const GLfloat* v) { ffp_vertex4(v[0], v[1], 0, 1); }
void glVertex3fv(const GLfloat* v) { ffp_vertex4(v[0], v[1], v[2], 1); }
void glVertex4fv(const GLfloat* v) { ffp_vertex4(v[0], v[1], v[2], v[3]); }
void glVertex2dv(const GLdouble* v) { glVertex2d(v[0], v[1]); }
void glVertex3dv(const GLdouble* v) { glVertex3d(v[0], v[1], v[2]); }
void glVertex4dv(const GLdouble* v) { glVertex4d(v[0], v[1], v[2], v[3]); }
void glVertex2iv(const GLint* v) { glVertex2i(v[0], v[1]); }
void glVertex3iv(const GLint* v) { glVertex3i(v[0], v[1], v[2]); }
void glVertex4iv(const GLint* v) { glVertex4i(v[0], v[1], v[2], v[3]); }
void glVertex2sv(const GLshort* v) { glVertex2s(v[0], v[1]); }
void glVertex3sv(const GLshort* v) { glVertex3s(v[0], v[1], v[2]); }
void glVertex4sv(const GLshort* v) { glVertex4s(v[0], v[1], v[2], v[3]); }

/* ------------------------------------------------------------------------- */
/* Client arrays                                                             */
/* ------------------------------------------------------------------------- */

void glClientActiveTexture(GLenum texture) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.client_active_texture = texture;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.vertex_array.size = size; c->state.vertex_array.type = type;
    c->state.vertex_array.stride = stride; c->state.vertex_array.ptr = pointer;
    ffp_apply_arrays(c);
}
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.normal_array.size = 3; c->state.normal_array.type = type;
    c->state.normal_array.stride = stride; c->state.normal_array.ptr = pointer;
    ffp_apply_arrays(c);
}
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.color_array.size = size; c->state.color_array.type = type;
    c->state.color_array.stride = stride; c->state.color_array.ptr = pointer;
    ffp_apply_arrays(c);
}
void glSecondaryColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.secondary_array.size = size; c->state.secondary_array.type = type;
    c->state.secondary_array.stride = stride; c->state.secondary_array.ptr = pointer;
    ffp_apply_arrays(c);
}
void glFogCoordPointer(GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    c->state.fog_array.size = 1; c->state.fog_array.type = type;
    c->state.fog_array.stride = stride; c->state.fog_array.ptr = pointer;
    ffp_apply_arrays(c);
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    ffp_context_t* c = ffp_ensure();
    int u;
    if (!c) return;
    u = c->state.client_active_texture - GL_TEXTURE0;
    if (u < 0 || u >= FFP_MAX_TEX) u = 0;
    c->state.texcoord_array[u].size = size; c->state.texcoord_array[u].type = type;
    c->state.texcoord_array[u].stride = stride; c->state.texcoord_array[u].ptr = pointer;
    ffp_apply_arrays(c);
}

static void ffp_set_client_array_enabled(int which, bool enabled) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    switch (which) {
        case 0: c->state.vertex_array.enabled = enabled; break;
        case 1: c->state.normal_array.enabled = enabled; break;
        case 2: c->state.color_array.enabled = enabled; break;
        case 3: c->state.secondary_array.enabled = enabled; break;
        case 4: c->state.fog_array.enabled = enabled; break;
        default: {
            int u = c->state.client_active_texture - GL_TEXTURE0;
            if (u < 0 || u >= FFP_MAX_TEX) u = 0;
            c->state.texcoord_array[u].enabled = enabled;
            break;
        }
    }
    ffp_apply_arrays(c);
}

void glEnableClientState(GLenum array) {
    ffp_context_t* c = ffp_ensure();
    if (!c) return;
    if (c->recording) { ffp_record(c, 2, (int)array, 1, 0, NULL, 0); }
    switch (array) {
        case GL_VERTEX_ARRAY: ffp_set_client_array_enabled(0, true); break;
        case GL_NORMAL_ARRAY: ffp_set_client_array_enabled(1, true); break;
        case GL_COLOR_ARRAY: ffp_set_client_array_enabled(2, true); break;
        case GL_SECONDARY_COLOR_ARRAY: ffp_set_client_array_enabled(3, true); break;
        case GL_FOG_COORD_ARRAY: ffp_set_client_array_enabled(4, true); break;
        case GL_TEXTURE_COORD_ARRAY: ffp_set_client_array_enabled(5, true); break;
        default: break;
    }
}

void glDisableClientState(GLenum array) {
    ffp_context_t* c = FFP;
    if (!c) return;
    if (c->recording) { ffp_record(c, 2, (int)array, 0, 0, NULL, 0); }
    switch (array) {
        case GL_VERTEX_ARRAY: ffp_set_client_array_enabled(0, false); break;
        case GL_NORMAL_ARRAY: ffp_set_client_array_enabled(1, false); break;
        case GL_COLOR_ARRAY: ffp_set_client_array_enabled(2, false); break;
        case GL_SECONDARY_COLOR_ARRAY: ffp_set_client_array_enabled(3, false); break;
        case GL_FOG_COORD_ARRAY: ffp_set_client_array_enabled(4, false); break;
        case GL_TEXTURE_COORD_ARRAY: ffp_set_client_array_enabled(5, false); break;
        default: break;
    }
}

void glGetPointerv(GLenum pname, void** params) {
    ffp_context_t* c = FFP;
    if (!c || !params) return;
    switch (pname) {
        case GL_VERTEX_ARRAY_POINTER: *params = (void*)c->state.vertex_array.ptr; break;
        case GL_NORMAL_ARRAY_POINTER: *params = (void*)c->state.normal_array.ptr; break;
        case GL_COLOR_ARRAY_POINTER: *params = (void*)c->state.color_array.ptr; break;
        case GL_SECONDARY_COLOR_ARRAY_POINTER: *params = (void*)c->state.secondary_array.ptr; break;
        case GL_FOG_COORD_ARRAY_POINTER: *params = (void*)c->state.fog_array.ptr; break;
        case GL_TEXTURE_COORD_ARRAY_POINTER: {
            int u = c->state.client_active_texture - GL_TEXTURE0;
            if (u < 0 || u >= FFP_MAX_TEX) u = 0;
            *params = (void*)c->state.texcoord_array[u].ptr;
            break;
        }
        default: *params = NULL; break;
    }
}

/* ------------------------------------------------------------------------- */
/* Draw API                                                                  */
/* ------------------------------------------------------------------------- */

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    ffp_context_t* c = FFP;
    if (!c || !c->ff_in_use) { es3_functions.glDrawArrays(mode, first, count); return; }
    ffp_begin_draw(c);
    if (c->program != 0) ffp_issue(mode, count, false, 0, NULL, first);
    ffp_end_draw();
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices) {
    ffp_context_t* c = FFP;
    if (!c || !c->ff_in_use) { es3_functions.glDrawElements(mode, count, type, indices); return; }
    ffp_begin_draw(c);
    if (c->program != 0) ffp_issue(mode, count, true, type, indices, 0);
    ffp_end_draw();
}

void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid* indices) {
    ffp_context_t* c = FFP;
    if (!c || !c->ff_in_use) { es3_functions.glDrawRangeElements(mode, start, end, count, type, indices); return; }
    ffp_begin_draw(c);
    if (c->program != 0) ffp_issue(mode, count, true, type, indices, 0);
    ffp_end_draw();
}

static void ffp_fetch(GLenum type, const void* base, GLsizei stride, GLint index, int comps, float* out, bool normalized) {
    const unsigned char* p;
    int tight = (stride == 0);
    int c;
    if (!base || comps <= 0) return;
    for (c = 0; c < comps; c++) out[c] = 0.0f;
    if (comps == 4) out[3] = 1.0f;
    switch (type) {
        case GL_FLOAT: {
            int es = 4;
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * es : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = ((const float*)p)[c];
            break;
        }
        case GL_DOUBLE: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * 8 : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = (float)((const GLdouble*)p)[c];
            break;
        }
        case GL_BYTE: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLbyte*)p)[c] / 127.0f : (float)((const GLbyte*)p)[c];
            break;
        }
        case GL_UNSIGNED_BYTE: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLubyte*)p)[c] / 255.0f : (float)((const GLubyte*)p)[c];
            break;
        }
        case GL_SHORT: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * 2 : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLshort*)p)[c] / 32767.0f : (float)((const GLshort*)p)[c];
            break;
        }
        case GL_UNSIGNED_SHORT: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * 2 : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLushort*)p)[c] / 65535.0f : (float)((const GLushort*)p)[c];
            break;
        }
        case GL_INT: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * 4 : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLint*)p)[c] / 2147483647.0f : (float)((const GLint*)p)[c];
            break;
        }
        case GL_UNSIGNED_INT: {
            p = (const unsigned char*)base + (tight ? (size_t)index * comps * 4 : (size_t)index * stride);
            for (c = 0; c < comps; c++) out[c] = normalized ? ((const GLuint*)p)[c] / 4294967295.0f : (float)((const GLuint*)p)[c];
            break;
        }
        default: break;
    }
}

void glArrayElement(GLint i) {
    ffp_context_t* c = FFP;
    ffp_state_t* s;
    float p[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float tmp[4];
    int u;
    if (!c) return;
    FFP_MARK(c);
    s = &c->state;
    if (s->vertex_array.enabled) {
        ffp_fetch(s->vertex_array.type, s->vertex_array.ptr, s->vertex_array.stride, i, s->vertex_array.size, p, false);
        if (s->vertex_array.size < 3) p[2] = 0.0f;
        if (s->vertex_array.size < 4) p[3] = 1.0f;
    }
    if (s->normal_array.enabled) {
        ffp_fetch(s->normal_array.type, s->normal_array.ptr, s->normal_array.stride, i, 3, tmp, true);
        s->current_normal[0] = tmp[0]; s->current_normal[1] = tmp[1]; s->current_normal[2] = tmp[2];
    }
    if (s->color_array.enabled) {
        ffp_fetch(s->color_array.type, s->color_array.ptr, s->color_array.stride, i, s->color_array.size, tmp, true);
        if (s->color_array.size < 4) tmp[3] = 1.0f;
        s->current_color[0] = tmp[0]; s->current_color[1] = tmp[1]; s->current_color[2] = tmp[2]; s->current_color[3] = tmp[3];
    }
    if (s->secondary_array.enabled) {
        ffp_fetch(s->secondary_array.type, s->secondary_array.ptr, s->secondary_array.stride, i, 3, tmp, true);
        s->current_secondary[0] = tmp[0]; s->current_secondary[1] = tmp[1]; s->current_secondary[2] = tmp[2];
    }
    if (s->fog_array.enabled) {
        ffp_fetch(s->fog_array.type, s->fog_array.ptr, s->fog_array.stride, i, 1, tmp, false);
        s->current_fogcoord = tmp[0];
    }
    for (u = 0; u < FFP_MAX_TEX; u++) {
        if (s->texcoord_array[u].enabled) {
            ffp_fetch(s->texcoord_array[u].type, s->texcoord_array[u].ptr, s->texcoord_array[u].stride, i, s->texcoord_array[u].size, tmp, false);
            if (s->texcoord_array[u].size < 4) tmp[3] = 1.0f;
            s->current_texcoord[u][0] = tmp[0]; s->current_texcoord[u][1] = tmp[1];
            s->current_texcoord[u][2] = tmp[2]; s->current_texcoord[u][3] = tmp[3];
        }
    }
    ffp_imm_push(c, p);
}

/* ------------------------------------------------------------------------- */
/* Lighting / material                                                       */
/* ------------------------------------------------------------------------- */

static void ffp_transform_light_pos(ffp_context_t* c, const float* in, float* out) {
    const float* m = c->state.modelview;
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12] * in[3];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13] * in[3];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
    out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

static void ffp_transform_dir(ffp_context_t* c, const float* in, float* out) {
    const float* m = c->state.modelview;
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2];
}

static void ffp_light_common(GLenum light, GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    int i;
    if (!c) return;
    FFP_MARK(c);
    i = (int)light - GL_LIGHT0;
    if (i < 0 || i >= FFP_MAX_LIGHTS) return;
    if (c->recording) { ffp_record(c, 60, (int)light, (int)pname, 0, params, 4); return; }
    switch (pname) {
        case GL_AMBIENT: memcpy(c->state.light.ambient[i], params, sizeof(float) * 4); break;
        case GL_DIFFUSE: memcpy(c->state.light.diffuse[i], params, sizeof(float) * 4); break;
        case GL_SPECULAR: memcpy(c->state.light.specular[i], params, sizeof(float) * 4); break;
        case GL_POSITION: {
            float tmp[4];
            ffp_transform_light_pos(c, params, tmp);
            memcpy(c->state.light.position[i], tmp, sizeof(float) * 4);
            break;
        }
        case GL_SPOT_DIRECTION: {
            float tmp[3];
            ffp_transform_dir(c, params, tmp);
            memcpy(c->state.light.spot_dir[i], tmp, sizeof(float) * 3);
            break;
        }
        case GL_SPOT_EXPONENT: c->state.light.spot_exp[i] = params[0]; break;
        case GL_SPOT_CUTOFF:
            c->state.light.spot_cos[i] = cosf(params[0] * (FFP_PI / 180.0f));
            c->state.light.spotlight[i] = (params[0] < 90.0f && params[0] >= 0.0f) ? 1 : 0;
            break;
        case GL_CONSTANT_ATTENUATION: c->state.light.atten[i][0] = params[0]; break;
        case GL_LINEAR_ATTENUATION: c->state.light.atten[i][1] = params[0]; break;
        case GL_QUADRATIC_ATTENUATION: c->state.light.atten[i][2] = params[0]; break;
        default: break;
    }
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
    float p[4] = { param, 0, 0, 1 };
    if (pname == GL_SPOT_DIRECTION) { p[0] = param; p[1] = 0; p[2] = 0; }
    ffp_light_common(light, pname, p);
}
void glLighti(GLenum light, GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 1 }; ffp_light_common(light, pname, p); }
void glLightfv(GLenum light, GLenum pname, const GLfloat* params) { ffp_light_common(light, pname, params); }
void glLightiv(GLenum light, GLenum pname, const GLint* params) { float p[4]; int n = (pname == GL_POSITION || pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR) ? 4 : 3; for (int i = 0; i < n; i++) p[i] = (float)params[i]; ffp_light_common(light, pname, p); }

static void ffp_light_model_common(GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 61, (int)pname, 0, 0, params, 4); return; }
    switch (pname) {
        case GL_LIGHT_MODEL_AMBIENT: memcpy(c->state.light.model_ambient, params, sizeof(float) * 4); break;
        case GL_LIGHT_MODEL_LOCAL_VIEWER: c->state.light.local_viewer = params[0] != 0.0f; break;
        case GL_LIGHT_MODEL_TWO_SIDE: c->state.light.two_side = params[0] != 0.0f; break;
        case GL_LIGHT_MODEL_COLOR_CONTROL: c->state.color_sum = (params[0] == GL_SEPARATE_SPECULAR_COLOR); break;
        default: break;
    }
}
void glLightModelf(GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 0 }; ffp_light_model_common(pname, p); }
void glLightModeli(GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 0 }; ffp_light_model_common(pname, p); }
void glLightModelfv(GLenum pname, const GLfloat* params) { ffp_light_model_common(pname, params); }
void glLightModeliv(GLenum pname, const GLint* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_light_model_common(pname, p); }

static void ffp_material_common(GLenum face, GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    ffp_material_t* mf;
    ffp_material_t* mb;
    if (!c) return;
    mf = &c->state.material_front;
    mb = &c->state.material_back;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 62, (int)face, (int)pname, 0, params, 4); return; }
    {
        int fronts = (face == GL_FRONT || face == GL_FRONT_AND_BACK);
        int backs = (face == GL_BACK || face == GL_FRONT_AND_BACK);
        switch (pname) {
            case GL_AMBIENT:
                if (fronts) memcpy(mf->ambient, params, sizeof(float) * 4);
                if (backs) memcpy(mb->ambient, params, sizeof(float) * 4);
                break;
            case GL_DIFFUSE:
                if (fronts) memcpy(mf->diffuse, params, sizeof(float) * 4);
                if (backs) memcpy(mb->diffuse, params, sizeof(float) * 4);
                break;
            case GL_SPECULAR:
                if (fronts) memcpy(mf->specular, params, sizeof(float) * 4);
                if (backs) memcpy(mb->specular, params, sizeof(float) * 4);
                break;
            case GL_EMISSION:
                if (fronts) memcpy(mf->emission, params, sizeof(float) * 4);
                if (backs) memcpy(mb->emission, params, sizeof(float) * 4);
                break;
            case GL_SHININESS:
                if (fronts) mf->shininess = params[0];
                if (backs) mb->shininess = params[0];
                break;
            case GL_AMBIENT_AND_DIFFUSE:
                if (fronts) { memcpy(mf->ambient, params, sizeof(float) * 4); memcpy(mf->diffuse, params, sizeof(float) * 4); }
                if (backs) { memcpy(mb->ambient, params, sizeof(float) * 4); memcpy(mb->diffuse, params, sizeof(float) * 4); }
                break;
            default: break;
        }
    }
}
void glMaterialf(GLenum face, GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 1 }; ffp_material_common(face, pname, p); }
void glMateriali(GLenum face, GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 1 }; ffp_material_common(face, pname, p); }
void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params) { ffp_material_common(face, pname, params); }
void glMaterialiv(GLenum face, GLenum pname, const GLint* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_material_common(face, pname, p); }

void glColorMaterial(GLenum face, GLenum mode) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 63, (int)face, (int)mode, 0, NULL, 0); return; }
    c->state.color_material_face = face;
    c->state.color_material_mode = mode;
}

void glShadeModel(GLenum mode) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 64, (int)mode, 0, 0, NULL, 0); return; }
    c->state.shade_model = mode;
}

/* ------------------------------------------------------------------------- */
/* Fog / alpha test                                                          */
/* ------------------------------------------------------------------------- */

static void ffp_fog_common(GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 68, (int)pname, 0, 0, params, 4); return; }
    switch (pname) {
        case GL_FOG_MODE: c->state.fog_mode = (int)params[0]; break;
        case GL_FOG_COLOR: memcpy(c->state.fog_color, params, sizeof(float) * 4); break;
        case GL_FOG_DENSITY: c->state.fog_density = params[0]; break;
        case GL_FOG_START: c->state.fog_start = params[0]; break;
        case GL_FOG_END: c->state.fog_end = params[0]; break;
        case GL_FOG_INDEX: break;
        case GL_FOG_COORD_SRC: c->state.fog_coord_src = (int)params[0]; break;
        default: break;
    }
}
void glFogf(GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 1 }; ffp_fog_common(pname, p); }
void glFogi(GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 1 }; ffp_fog_common(pname, p); }
void glFogfv(GLenum pname, const GLfloat* params) { ffp_fog_common(pname, params); }
void glFogiv(GLenum pname, const GLint* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_fog_common(pname, p); }

void glAlphaFunc(GLenum func, GLclampf ref) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { float f[1] = { (float)ref }; ffp_record(c, 67, (int)func, 0, 0, f, 1); return; }
    c->state.alpha_func = func;
    c->state.alpha_ref = ref;
}

/* ------------------------------------------------------------------------- */
/* Texture environment / coordinate generation                               */
/* ------------------------------------------------------------------------- */

static int ffp_active_unit(ffp_context_t* c) {
    int u = c->state.active_texture - GL_TEXTURE0;
    if (u < 0 || u >= FFP_MAX_TEX) u = 0;
    return u;
}

static void ffp_texenv_common(GLenum target, GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    int u;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 65, (int)target, (int)pname, 0, params, 4); return; }
    u = ffp_active_unit(c);
    switch (pname) {
        case GL_TEXTURE_ENV_MODE: c->state.tex.env_mode[u] = (int)params[0]; break;
        case GL_TEXTURE_ENV_COLOR: memcpy(c->state.tex.env_color[u], params, sizeof(float) * 4); break;
        default: break;
    }
}
void glTexEnvf(GLenum target, GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 0 }; ffp_texenv_common(target, pname, p); }
void glTexEnvi(GLenum target, GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 0 }; ffp_texenv_common(target, pname, p); }
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat* params) { ffp_texenv_common(target, pname, params); }
void glTexEnviv(GLenum target, GLenum pname, const GLint* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_texenv_common(target, pname, p); }

static int ffp_texgen_mode_from_enum(GLenum mode) {
    switch (mode) {
        case GL_OBJECT_LINEAR: return 1;
        case GL_EYE_LINEAR: return 2;
        case GL_SPHERE_MAP: return 3;
        case GL_REFLECTION_MAP: return 4;
        case GL_NORMAL_MAP: return 5;
        default: return 0;
    }
}

static void ffp_texgen_common(GLenum coord, GLenum pname, const float* params) {
    ffp_context_t* c = FFP;
    int u, comp;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 66, (int)coord, (int)pname, 0, params, 4); return; }
    u = ffp_active_unit(c);
    if (coord == GL_S) comp = 0;
    else if (coord == GL_T) comp = 1;
    else if (coord == GL_R) comp = 2;
    else if (coord == GL_Q) comp = 3;
    else return;
    if (pname == GL_TEXTURE_GEN_MODE) {
        c->state.tex.gen_mode[comp][u] = ffp_texgen_mode_from_enum((GLenum)params[0]);
    } else if (pname == GL_OBJECT_PLANE) {
        memcpy(c->state.tex.gen_object[comp][u], params, sizeof(float) * 4);
    } else if (pname == GL_EYE_PLANE) {
        float tmp[4];
        /* Eye plane is transformed by the inverse of the modelview at set time.
         * Approximation: transform by transpose of modelview (works for rigid
         * transforms) to keep the implementation simple. */
        const float* m = c->state.modelview;
        tmp[0] = m[0] * params[0] + m[1] * params[1] + m[2] * params[2];
        tmp[1] = m[4] * params[0] + m[5] * params[1] + m[6] * params[2];
        tmp[2] = m[8] * params[0] + m[9] * params[1] + m[10] * params[2];
        tmp[3] = params[3];
        memcpy(c->state.tex.gen_eye[comp][u], tmp, sizeof(float) * 4);
    }
}
void glTexGenf(GLenum coord, GLenum pname, GLfloat param) { float p[4] = { param, 0, 0, 0 }; ffp_texgen_common(coord, pname, p); }
void glTexGeni(GLenum coord, GLenum pname, GLint param) { float p[4] = { (float)param, 0, 0, 0 }; ffp_texgen_common(coord, pname, p); }
void glTexGenfv(GLenum coord, GLenum pname, const GLfloat* params) { ffp_texgen_common(coord, pname, params); }
void glTexGeniv(GLenum coord, GLenum pname, const GLint* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_texgen_common(coord, pname, p); }
void glTexGend(GLenum coord, GLenum pname, GLdouble param) { glTexGenf(coord, pname, (float)param); }
void glTexGendv(GLenum coord, GLenum pname, const GLdouble* params) { float p[4]; for (int i = 0; i < 4; i++) p[i] = (float)params[i]; ffp_texgen_common(coord, pname, p); }

void glGetTexEnviv(GLenum target, GLenum pname, GLint* params) {
    ffp_context_t* c = FFP;
    int u;
    if (!c) return;
    u = ffp_active_unit(c);
    if (pname == GL_TEXTURE_ENV_MODE) params[0] = c->state.tex.env_mode[u];
    else if (pname == GL_TEXTURE_ENV_COLOR) { params[0] = (GLint)c->state.tex.env_color[u][0]; params[1] = (GLint)c->state.tex.env_color[u][1]; params[2] = (GLint)c->state.tex.env_color[u][2]; params[3] = (GLint)c->state.tex.env_color[u][3]; }
}
void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat* params) {
    ffp_context_t* c = FFP;
    int u;
    if (!c) return;
    u = ffp_active_unit(c);
    if (pname == GL_TEXTURE_ENV_MODE) params[0] = (float)c->state.tex.env_mode[u];
    else if (pname == GL_TEXTURE_ENV_COLOR) memcpy(params, c->state.tex.env_color[u], sizeof(float) * 4);
}

/* ------------------------------------------------------------------------- */
/* Clip planes                                                               */
/* ------------------------------------------------------------------------- */

void glClipPlane(GLenum plane, const GLdouble* equation) {
    ffp_context_t* c = FFP;
    int p;
    float obj[4], eye[4];
    const float* m;
    if (!c) return;
    FFP_MARK(c);
    p = (int)plane - GL_CLIP_PLANE0;
    if (p < 0 || p >= FFP_MAX_CLIP) return;
    if (c->recording) {
        float f[4] = { (float)equation[0], (float)equation[1], (float)equation[2], (float)equation[3] };
        ffp_record(c, 69, (int)plane, 0, 0, f, 4);
        return;
    }
    obj[0] = (float)equation[0]; obj[1] = (float)equation[1];
    obj[2] = (float)equation[2]; obj[3] = (float)equation[3];
    memcpy(c->state.clip_plane_object[p], obj, sizeof(float) * 4);
    /* Store plane in eye space: plane_eye = plane_obj * inverse(M). For a rigid
     * transform the transpose of the upper 3x3 is the inverse. */
    m = c->state.modelview;
    eye[0] = m[0] * obj[0] + m[1] * obj[1] + m[2] * obj[2];
    eye[1] = m[4] * obj[0] + m[5] * obj[1] + m[6] * obj[2];
    eye[2] = m[8] * obj[0] + m[9] * obj[1] + m[10] * obj[2];
    eye[3] = obj[3] + (m[12] * obj[0] + m[13] * obj[1] + m[14] * obj[2]);
    memcpy(c->state.clip_plane[p], eye, sizeof(float) * 4);
}

void glGetClipPlane(GLenum plane, GLdouble* equation) {
    ffp_context_t* c = FFP;
    int p;
    if (!c) return;
    p = (int)plane - GL_CLIP_PLANE0;
    if (p < 0 || p >= FFP_MAX_CLIP) return;
    for (int i = 0; i < 4; i++) equation[i] = (GLdouble)c->state.clip_plane_object[p][i];
}

/* ------------------------------------------------------------------------- */
/* Active texture (tracked so FF state sees the right unit)                   */
/* ------------------------------------------------------------------------- */

void glActiveTexture(GLenum texture) {
    ffp_context_t* c = FFP;
    if (c) c->state.active_texture = texture;
    es3_functions.glActiveTexture(texture);
}

/* ------------------------------------------------------------------------- */
/* Attribute stack (push/pop)                                                */
/* ------------------------------------------------------------------------- */

static void ffp_push_state(ffp_context_t* c, bool client) {
    ffp_state_t** stack;
    int* top;
    int* cap;
    if (client) { stack = &c->client_stack; top = &c->client_stack_top; cap = &c->client_stack_capacity; }
    else { stack = &c->attrib_stack; top = &c->attrib_stack_top; cap = &c->attrib_stack_capacity; }
    if (*top >= *cap) {
        int newcap = *cap ? *cap * 2 : 8;
        ffp_state_t* ns = realloc(*stack, sizeof(ffp_state_t) * newcap);
        if (!ns) return;
        *stack = ns;
        *cap = newcap;
    }
    (*stack)[*top] = c->state;
    (*top)++;
}

static void ffp_pop_state(ffp_context_t* c, bool client) {
    ffp_state_t* stack;
    int* top;
    if (client) { stack = c->client_stack; top = &c->client_stack_top; }
    else { stack = c->attrib_stack; top = &c->attrib_stack_top; }
    if (*top <= 0) return;
    (*top)--;
    c->state = stack[*top];
}

void glPushAttrib(GLbitfield mask) { ffp_context_t* c = FFP; if (c) ffp_push_state(c, false); (void)mask; }
void glPopAttrib(void) { ffp_context_t* c = FFP; if (c) ffp_pop_state(c, false); }
void glPushClientAttrib(GLbitfield mask) { ffp_context_t* c = FFP; if (c) ffp_push_state(c, true); (void)mask; }
void glPopClientAttrib(void) { ffp_context_t* c = FFP; if (c) ffp_pop_state(c, true); }

/* ------------------------------------------------------------------------- */
/* Display lists                                                             */
/* ------------------------------------------------------------------------- */

static GLuint g_next_list = 1;

GLuint glGenLists(GLsizei range) {
    GLuint base = g_next_list;
    g_next_list += range;
    return base;
}

void glDeleteLists(GLuint list, GLsizei range) {
    ffp_context_t* c = FFP;
    int i;
    if (!c) return;
    for (i = 0; i < range; i++) {
        int id = (int)list + i;
        if (id >= 0 && id < c->dlists_capacity) {
            free(c->dlists[id].cmds);
            memset(&c->dlists[id], 0, sizeof(ffp_dlist_t));
        }
    }
}

GLboolean glIsList(GLuint list) {
    ffp_context_t* c = FFP;
    if (!c || (int)list >= c->dlists_capacity) return GL_FALSE;
    return c->dlists[list].used ? GL_TRUE : GL_FALSE;
}

void glNewList(GLuint list, GLenum mode) {
    ffp_context_t* c = ffp_ensure();
    int id = (int)list;
    if (!c) return;
    FFP_MARK(c);
    if (id >= c->dlists_capacity) {
        int newcap = c->dlists_capacity ? c->dlists_capacity * 2 : 64;
        while (id >= newcap) newcap *= 2;
        ffp_dlist_t* nd = realloc(c->dlists, sizeof(ffp_dlist_t) * newcap);
        if (!nd) return;
        memset(nd + c->dlists_capacity, 0, sizeof(ffp_dlist_t) * (newcap - c->dlists_capacity));
        c->dlists = nd;
        c->dlists_capacity = newcap;
    }
    if (mode == GL_COMPILE) { free(c->dlists[id].cmds); memset(&c->dlists[id], 0, sizeof(ffp_dlist_t)); }
    c->dlists[id].used = true;
    c->dlists[id].count = 0;
    c->recording = true;
    c->record_list = id;
}

void glEndList(void) {
    ffp_context_t* c = FFP;
    if (!c) return;
    c->recording = false;
    c->record_list = -1;
}

static void ffp_replay(ffp_context_t* c, int list) {
    ffp_dlist_t* dl;
    int i;
    bool saved = c->recording;
    if (list < 0 || list >= c->dlists_capacity) return;
    dl = &c->dlists[list];
    c->recording = false;
    for (i = 0; i < dl->count; i++) {
        ffp_cmd_t* cmd = &dl->cmds[i];
        switch (cmd->op) {
            case 1: ffp_handle_enable((GLenum)cmd->i0, (GLboolean)cmd->f[0]); break;
            case 2: if (cmd->i1) glEnableClientState((GLenum)cmd->i0); else glDisableClientState((GLenum)cmd->i0); break;
            case 10: glMatrixMode((GLenum)cmd->i0); break;
            case 11: glLoadIdentity(); break;
            case 12: glLoadMatrixf(cmd->f); break;
            case 13: glMultMatrixf(cmd->f); break;
            case 14: glPushMatrix(); break;
            case 15: glPopMatrix(); break;
            case 16: glOrtho(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3], cmd->f[4], cmd->f[5]); break;
            case 17: glFrustum(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3], cmd->f[4], cmd->f[5]); break;
            case 18: glTranslatef(cmd->f[0], cmd->f[1], cmd->f[2]); break;
            case 19: glScalef(cmd->f[0], cmd->f[1], cmd->f[2]); break;
            case 20: glRotatef(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
            case 30: glBegin((GLenum)cmd->i0); break;
            case 31: glEnd(); break;
            case 40: ffp_vertex4(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
            case 50: ffp_set_color4(cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
            case 51: ffp_set_normal3(cmd->f[0], cmd->f[1], cmd->f[2]); break;
            case 52: ffp_set_texcoord(cmd->i0, cmd->f[0], cmd->f[1], cmd->f[2], cmd->f[3]); break;
            case 53: glSecondaryColor3f(cmd->f[0], cmd->f[1], cmd->f[2]); break;
            case 54: glFogCoordf(cmd->f[0]); break;
            case 60: ffp_light_common((GLenum)cmd->i0, (GLenum)cmd->i1, cmd->f); break;
            case 61: ffp_light_model_common((GLenum)cmd->i0, cmd->f); break;
            case 62: ffp_material_common((GLenum)cmd->i0, (GLenum)cmd->i1, cmd->f); break;
            case 63: glColorMaterial((GLenum)cmd->i0, (GLenum)cmd->i1); break;
            case 64: glShadeModel((GLenum)cmd->i0); break;
            case 65: ffp_texenv_common((GLenum)cmd->i0, (GLenum)cmd->i1, cmd->f); break;
            case 66: ffp_texgen_common((GLenum)cmd->i0, (GLenum)cmd->i1, cmd->f); break;
            case 67: glAlphaFunc((GLenum)cmd->i0, cmd->f[0]); break;
            case 68: ffp_fog_common((GLenum)cmd->i0, cmd->f); break;
            case 69: { GLdouble eq[4]; eq[0] = cmd->f[0]; eq[1] = cmd->f[1]; eq[2] = cmd->f[2]; eq[3] = cmd->f[3]; glClipPlane((GLenum)cmd->i0, eq); break; }
            case 70: glActiveTexture((GLenum)cmd->i0); break;
            case 71: glClientActiveTexture((GLenum)cmd->i0); break;
            case 80: glCallList((GLuint)cmd->i0); break;
            case 81: glPushMatrix(); break;
            case 82: glPopMatrix(); break;
            default: break;
        }
    }
    c->recording = saved;
}

void glCallList(GLuint list) {
    ffp_context_t* c = FFP;
    if (!c) return;
    FFP_MARK(c);
    if (c->recording) { ffp_record(c, 80, (int)list, 0, 0, NULL, 0); return; }
    ffp_replay(c, (int)list);
}

void glCallLists(GLsizei n, GLenum type, const GLvoid* lists) {
    int i;
    for (i = 0; i < n; i++) {
        GLuint id;
        switch (type) {
            case GL_BYTE: id = ((const GLbyte*)lists)[i]; break;
            case GL_UNSIGNED_BYTE: id = ((const GLubyte*)lists)[i]; break;
            case GL_SHORT: id = ((const GLshort*)lists)[i]; break;
            case GL_UNSIGNED_SHORT: id = ((const GLushort*)lists)[i]; break;
            case GL_INT: id = ((const GLint*)lists)[i]; break;
            case GL_UNSIGNED_INT: id = ((const GLuint*)lists)[i]; break;
            case GL_FLOAT: id = (GLuint)((const GLfloat*)lists)[i]; break;
            default: id = 0; break;
        }
        glCallList(id);
    }
}

void glListBase(GLuint base) { (void)base; }

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

const char* ffp_get_version_string(void) {
    return "2.1 LTW (fixed-function compatibility profile)";
}
const char* ffp_get_shading_language_string(void) {
    return "1.20 LTW";
}
GLenum ffp_get_profile_mask(void) {
    return GL_CONTEXT_COMPATIBILITY_PROFILE_BIT;
}
