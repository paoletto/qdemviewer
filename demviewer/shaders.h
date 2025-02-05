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

uniform highp mat4 matData;
uniform highp mat4 matrix;
uniform highp float scale;

flat out int lineID;

void main()
{
    lineID = gl_VertexID / 2;
    gl_Position = matrix * matData * vertices[gl_VertexID] ;
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
uniform sampler2D dem;
uniform float minElevation;
float fetchDEM(ivec2 texelCoord) {
    return texelFetch(dem, texelCoord, 0).r;
}
uniform highp mat4 matrix;
uniform vec2 resolution;

uniform float elevationScale;
uniform int quadSplitDirection;
uniform float cOff; // coordinate offset, -0.5 || 0.5
uniform int samplingStride;
uniform int joined;

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

int sjoined = int(1.0 - cOff - 0.5); // 1 only when joined && !interactive
int ijoined = joined * int(!bool(sjoined));
int columnSize = int(resolution.y) - 1;
int res = int(resolution.x);
int splitDirectionOffset = quadSplitDirection * 6;
int rowSize = (res - 1);
float gridSpacing = 1.0 / float(res - 2 * sjoined); // because first and last are half-spaced in joined mode
vec4 gridScaling = vec4(gridSpacing,
                        gridSpacing,
                        1.0, 1.0);

vec4 neighbor(int id, int x, int y) {
    vec4 res = vertices[indices[id]];
    int iY = (columnSize - y - int(res.y)) * samplingStride + ijoined;
    int iX = (x + int(res.x)) * samplingStride + ijoined;
    const float elevation =  max(-10000000, fetchDEM(ivec2(iX,iY))) / elevationScale;
    res = (vec4(x + cOff / float(samplingStride),
                y + cOff / float(samplingStride), elevation, 0) + res) * gridScaling;
    res = clamp(res, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));
    return res;
}

void main()
{
    subQuadID = int(gl_VertexID / 6);
    const int x = subQuadID % rowSize;
    const int y = subQuadID / rowSize;

//    const vec2 texCoordScaling = vec2(1.0 / resolution.x
//                                     ,1.0 / resolution.y);

    const int triangleID = (gl_VertexID / 3) % 2;
    const int vertexID = (gl_VertexID % 6) + splitDirectionOffset;
    vec4 vertex = neighbor(vertexID, x,y);
    vec4 triVertex0 = neighbor(0 + 3 * triangleID + splitDirectionOffset, x,y);
    vec4 triVertex1 = neighbor(1 + 3 * triangleID + splitDirectionOffset, x,y);
    vec4 triVertex2 = neighbor(2 + 3 * triangleID + splitDirectionOffset, x,y);

    const vec3 first = triVertex2.xyz - triVertex0.xyz;
    const vec3 second = triVertex1.xyz - triVertex0.xyz;
    normal = normalize(cross(first, second));


//    texCoord = clamp(vec2(x + cOff, y + cOff) * texCoordScaling, vec2(0,0), vec2(1,1));
    texCoord = vertex.xy;
    gl_Position = matrix * vertex;
}
)";
static constexpr char vertexShaderTileJoinedDownsampled[] = R"(
#version 450 core
uniform sampler2D dem;
uniform float minElevation;
float fetchDEM(ivec2 texelCoord) {
    return texelFetch(dem, texelCoord, 0).r;
}
uniform highp mat4 matrix;

uniform vec2 resolution;

uniform float elevationScale;
uniform int quadSplitDirection;
//uniform float cOff; // coordinate offset, -0.5 || 0.5
uniform int samplingStride;


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

int joined = 1;
vec2 heightmapResolution = resolution * samplingStride + vec2(2,2);
float cOff = -0.5; // joined tiles have 0 at 0, 1 at 0.5


//int sjoined = int(1.0 - cOff - 0.5); // 1 only when joined && !interactive
//int ijoined = joined * int(!bool(sjoined));

int splitDirectionOffset = quadSplitDirection * 6;
int numPatchesX = (int(resolution.x) + 1);
float gridSpacing = 1.0 / float(heightmapResolution.x - 2); // * sjoined); // because first and last are half-spaced in joined mode
vec4 gridScaling = vec4(gridSpacing,
                        gridSpacing,
                        1.0, 1.0);

vec4 neighbor(int id, int x, int y) {
    vec4 res = vertices[indices[id]];

    const int iiY = clamp((y + int(res.y)) * samplingStride - samplingStride + 1
                   , 0
                   , int(heightmapResolution.y) - 1);
    const int iY = //clamp(
                    int(heightmapResolution.y) - 1 - iiY;
//                   , 0
//                   , int(heightmapResolution.y) - 1);

    const int iX = clamp((x + int(res.x)) * samplingStride - samplingStride + 1
                     , 0
                     , int(heightmapResolution.x) - 1);

    float elevation =  max(-10000000, fetchDEM(ivec2(iX,iY))) * elevationScale;
    res = vec4(float(iX) + cOff,
               float(iiY) + cOff,
               elevation,
               1) * gridScaling;
    res = clamp(res, vec4(0,0,-10000000,0), vec4(1,1,10000000,1));
    return res;
}

void main()
{
    subQuadID = int(gl_VertexID / 6);
    const int x = subQuadID % numPatchesX;
    const int y = subQuadID / numPatchesX;


    const int triangleID = (gl_VertexID / 3) % 2;
    const int vertexID = (gl_VertexID % 6) + splitDirectionOffset;
    vec4 vertex = neighbor(vertexID, x,y);
    vec4 triVertex0 = neighbor(0 + 3 * triangleID + splitDirectionOffset, x,y);
    vec4 triVertex1 = neighbor(1 + 3 * triangleID + splitDirectionOffset, x,y);
    vec4 triVertex2 = neighbor(2 + 3 * triangleID + splitDirectionOffset, x,y);

    const vec3 first = triVertex2.xyz - triVertex0.xyz;
    const vec3 second = triVertex1.xyz - triVertex0.xyz;
    normal = normalize(cross(first, second));
//    normal = normalize(vec3(-1,-1,0));

//    texCoord = clamp(vec2(x + cOff, y + cOff) * texCoordScaling, vec2(0,0), vec2(1,1));
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

static constexpr char fragmentShaderTileTextureArrayd[] = R"(
#version 450 core
uniform highp vec4 color;
uniform sampler2DArray rasterArray;
uniform int numSubtiles;
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
        float sideLength = sqrt(numSubtiles);
        vec2 scaled = texCoord * float(sideLength);
        vec2 subTexCoord;
        vec2 integral;
        subTexCoord  = modf(scaled, integral);
        float layer = (sideLength - integral.y - 1) * sideLength + integral.x;
        fragColor = textureGrad(rasterArray, vec3(subTexCoord, layer), dFdx(scaled), dFdy(scaled));

        fragColor *= vec4(vec3(lightColor.rgb) * vec3(diff * brightness), 1); // ToDo: move lighting controls to GUI
    } else {   // Fragment is back facing fragment
        fragColor = vec4(0.5,0.1,0.1,1) + diff * 0.2;
    }
}
)";

}
