#include "Uniforms.glsl"
#include "Samplers.glsl"
#include "Transform.glsl"

varying vec2 vTexCoord;
varying vec3 vWorldDir;

#ifdef COMPILEPS
uniform float cCloudAngle;
uniform float cNightFactor;

vec3 RotateAroundAxis(vec3 dir, vec3 axis, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return dir * c + cross(axis, dir) * s + axis * dot(axis, dir) * (1.0 - c);
}

vec3 RotateClouds(vec3 dir)
{
    const vec3 axis = normalize(vec3(0.12, 1.0, 0.07));
    return RotateAroundAxis(dir, axis, cCloudAngle);
}
#endif

void VS()
{
    mat4 modelMatrix = iModelMatrix;
    vec3 worldPos = GetWorldPos(modelMatrix);
    gl_Position = GetClipPos(worldPos);
    vTexCoord = iTexCoord.xy;
    // Direction from camera to this vertex for cubemap lookup
    vWorldDir = normalize(worldPos - cCameraPos);
}

void PS()
{
    vec4 ufoTex = texture2D(sDiffMap, vTexCoord);
    if (ufoTex.a < 0.1)
        discard;

    // Sample the skybox cloud cubemap at this fragment's direction
    vec3 cloudDir = RotateClouds(vWorldDir);
    float cloudAlpha = textureCube(sEnvCubeMap, cloudDir).a;

    // Hide behind any cloud cover: even light haze hides the UFO
    float visibility = 1.0 - smoothstep(0.05, 0.25, cloudAlpha);

    // Fade out at night
    visibility *= 1.0 - cNightFactor;

    if (visibility < 0.01)
        discard;

    gl_FragColor = vec4(ufoTex.rgb, ufoTex.a * visibility);
}
