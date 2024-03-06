/****************************************************************************
**
** Copyright (C) 2024- Paolo Angelelli <paoletto@gmail.com>
**
** Commercial License Usage
** Licensees holding a valid commercial qdemviewer license may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement with the copyright holder. For licensing terms
** and conditions and further information contact the copyright holder.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3. The licenses are as published by
** the Free Software Foundation at https://www.gnu.org/licenses/gpl-3.0.html,
** with the exception that the use of this work for training artificial intelligence
** is prohibited for both commercial and non-commercial use.
**
****************************************************************************/

namespace  {
static constexpr char vertexShaderOrigin[] = R"(
#version 450 core
const vec4 vertices[12] = {
     vec4(-1, 0, 0,1)
    ,vec4(0,0,0,1)
    ,vec4(0,0,0,1)
    ,vec4( 1, 0, 0,1)
    ,vec4( 0,-1, 0,1)
    ,vec4(0,0,0,1)
    ,vec4(0,0,0,1)
    ,vec4( 0, 1, 0,1)
    ,vec4( 0, 0,-1,1)
    ,vec4(0,0,0,1)
    ,vec4(0,0,0,1)
    ,vec4( 0, 0, 1,1)
};

uniform highp mat4 matrix;
uniform highp float scale;

flat out int lineID;

void main()
{
//    gl_Position = projection * view * model * vert;
    lineID = gl_VertexID / 2;
    gl_Position = matrix * vertices[gl_VertexID];
}
)";
static constexpr char fragmentShaderOrigin[] = R"(
#version 450 core

const vec4 colors[6] = {
     vec4(1, 0, 0, 1)
    ,vec4(1, 0.4, 0.4, 1)
    ,vec4(0, 1, 0, 1)
    ,vec4(0.4, 1, 0.4, 1)
    ,vec4(0, 0, 1, 1)
    ,vec4(0.4, 0.4, 1, 1)
};

flat in int lineID;
out vec4 fragColor;

void main()
{
    fragColor = colors[lineID];
}
)";

static constexpr char vertexShaderTile[] = R"(
#version 450 core
#define IMGFMT r32f
layout(binding=0, IMGFMT) uniform readonly highp image2D dem;
uniform highp mat4 matrix;
uniform vec2 resolution;
uniform vec2 lowRes;

uniform float elevationScale;
uniform int quadSplitDirection;

flat out int subQuadID;
flat out vec3 normal;
smooth out vec2 texCoord;
const int indices[12] = {2,1,0,0,3,2, 3,1,0,3,2,1}; // TODO: choose quad splitting orientation based on light direction?
const vec4 vertices[4] = {
     vec4(0,0,0,1)
    ,vec4(0,1,0,1)
    ,vec4(1,1,0,1)
    ,vec4(1,0,0,1)
};

void main()
{
    const int splitDirectionOffset = quadSplitDirection * 6;
    const int res = int(resolution.x);
    const int rowSize = (res - 1);
    const int columnSize = int(resolution.y) - 1;
    const float gridSpacing = 1.0 / float(res);

    subQuadID = int(gl_VertexID / 6);
    const int x = subQuadID % rowSize;
    const int y = subQuadID / rowSize;

    const vec4 gridScaling = vec4(gridSpacing,
                                  gridSpacing,
                                  1.0, 1.0);
    const vec2 texCoordScaling = vec2(1.0 / resolution.x
                                      ,1.0 / resolution.y);

    const int triangleID = (gl_VertexID / 3) % 2;
    int vertexID = gl_VertexID % 6;
    vertexID = indices[vertexID + splitDirectionOffset];

    vec4 vertex = vertices[vertexID];

    int iY = columnSize - y - int(vertex.y);
    int iX = x + int(vertex.x);
    const float elevation =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;

    vec4 intVertex = (vec4(x + 0.5, y + 0.5, elevation, 0) + vertex);
    vertex = intVertex * gridScaling;

    vec4 triVertex0 = vertices[indices[0 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex0.y);
    iX = x + int(triVertex0.x);
    const float elevation1 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex0 = (vec4(x + 0.5, y + 0.5, elevation1, 0) + triVertex0) * gridScaling;

    vec4 triVertex1 = vertices[indices[1 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex1.y);
    iX = x + int(triVertex1.x);
    const float elevation2 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex1 = (vec4(x + 0.5, y + 0.5, elevation2, 0) + triVertex1) * gridScaling;

    vec4 triVertex2 = vertices[indices[2 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex2.y);
    iX = x + int(triVertex2.x);
    const float elevation3 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex2 = (vec4(x + 0.5, y + 0.5, elevation3, 0) + triVertex2) * gridScaling;

    const vec3 first = triVertex2.xyz - triVertex0.xyz;
    const vec3 second = triVertex1.xyz - triVertex0.xyz;
    normal = normalize(cross(first, second));

    texCoord = intVertex.xy * texCoordScaling;
    gl_Position = matrix * vertex;
}
)";
static constexpr char vertexShaderTileFused[] = R"(
#version 450 core
#define IMGFMT r32f
layout(binding=0, IMGFMT) uniform readonly highp image2D dem;
uniform highp mat4 matrix;
uniform vec2 resolution;
uniform vec2 lowRes;

uniform float elevationScale;
uniform int quadSplitDirection;

flat out int subQuadID;
flat out vec3 normal;
smooth out vec2 texCoord;
const int indices[12] = {2,1,0,0,3,2, 3,1,0,3,2,1}; // TODO: choose quad splitting orientation based on light direction?
const vec4 vertices[4] = {
     vec4(0,0,0,1)
    ,vec4(0,1,0,1)
    ,vec4(1,1,0,1)
    ,vec4(1,0,0,1)
};

void main()
{
    const int splitDirectionOffset = quadSplitDirection * 6;
    const int res = int(resolution.x);
    const int rowSize = (res - 1);
    const int columnSize = int(resolution.y) - 1;
    const float gridSpacing = 1.0 / float(res - 2); // because first and last are half-spaced

    subQuadID = int(gl_VertexID / 6);
    const int x = subQuadID % rowSize;
    const int y = subQuadID / rowSize;

    const vec4 gridScaling = vec4(gridSpacing,
                                  gridSpacing,
                                  1.0, 1.0);
    const vec2 texCoordScaling = vec2(1.0 / resolution.x
                                      ,1.0 / resolution.y);

    const int triangleID = (gl_VertexID / 3) % 2;
    int vertexID = gl_VertexID % 6;
    vertexID = indices[vertexID + splitDirectionOffset];

    vec4 vertex = vertices[vertexID];

    int iY = columnSize - y - int(vertex.y);
    int iX = x + int(vertex.x);
    const float elevation =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;

    vec4 intVertex = (vec4(x - 0.5, y - 0.5, elevation, 0) + vertex);
    vertex = clamp(intVertex * gridScaling, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));

    vec4 triVertex0 = vertices[indices[0 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex0.y);
    iX = x + int(triVertex0.x);
    const float elevation1 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex0 = (vec4(x - 0.5, y - 0.5, elevation1, 0) + triVertex0) * gridScaling;
    triVertex0 = clamp(triVertex0, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));

    vec4 triVertex1 = vertices[indices[1 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex1.y);
    iX = x + int(triVertex1.x);
    const float elevation2 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex1 = (vec4(x - 0.5, y - 0.5, elevation2, 0) + triVertex1) * gridScaling;
    triVertex1 = clamp(triVertex1, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));

    vec4 triVertex2 = vertices[indices[2 + 3 * triangleID + splitDirectionOffset]];
    iY = columnSize - y - int(triVertex2.y);
    iX = x + int(triVertex2.x);
    const float elevation3 =  max(-10000000, imageLoad(dem, ivec2(iX,iY)).r) / elevationScale;
    triVertex2 = (vec4(x - 0.5, y - 0.5, elevation3, 0) + triVertex2) * gridScaling;
    triVertex2 = clamp(triVertex2, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));

    const vec3 first = triVertex2.xyz - triVertex0.xyz;
    const vec3 second = triVertex1.xyz - triVertex0.xyz;
    normal = normalize(cross(first, second));

    texCoord = intVertex.xy * texCoordScaling;
    texCoord = vertex.xy;
    gl_Position = matrix * vertex;
}
)";
static constexpr char fragmentShaderTile[] = R"(
#version 450 core
uniform highp vec4 color;
flat in int subQuadID;
flat in vec3 normal;
smooth in vec2 TexCoord;
uniform vec3 lightDirection;

//const vec3 lightDir = normalize(vec3(0.2,-0.2,-1));
vec3 lightDir = lightDirection;
const vec4 lightColor = vec4(1,1,1,1);

out vec4 fragColor;
void main()
{
    float diff = max(dot(normal, lightDir), 0.0);
    if (gl_FrontFacing) {
        fragColor = color * vec4(vec3(diff), 1);
    } else {   // Fragment is back facing fragment
        fragColor = vec4(0.5,0.1,0.1,1) + diff * 0.2;
    }
}
)";

static constexpr char fragmentShaderTileTextured[] = R"(
#version 450 core
uniform highp vec4 color;
uniform sampler2D raster;
//uniform float[256] lightingCurve;
uniform float brightness;
uniform vec3 lightDirection;

flat in int subQuadID;
flat in vec3 normal;
smooth in vec2 texCoord;

//const vec3 lightDir = normalize(vec3(0.2,-0.2,-1));
vec3 lightDir = lightDirection;
vec4 lightColor = color;

out vec4 fragColor;
void main()
{

    float diff = max(dot(normal, lightDir), 0.0);
    if (gl_FrontFacing) {
        fragColor = texture(raster, texCoord);
        fragColor *= vec4(vec3(lightColor.rgb) * vec3(diff * brightness), 1); // ToDo: move lighting controls to GUI
    } else {   // Fragment is back facing fragment
        fragColor = vec4(0.5,0.1,0.1,1) + diff * 0.2;
    }
}
)";

}
