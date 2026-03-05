#include "Uniforms.glsl"
#include "Samplers.glsl"
#include "Transform.glsl"
#include "ScreenPos.glsl"
#include "Fog.glsl"

#ifndef GL_ES
varying vec4 vScreenPos;
varying vec2 vReflectUV;
varying vec2 vWaterUV;
varying vec4 vEyeVec;
#else
varying highp vec4 vScreenPos;
varying highp vec2 vReflectUV;
varying highp vec2 vWaterUV;
varying highp vec4 vEyeVec;
#endif
varying vec3 vNormal;

#ifdef COMPILEVS
uniform vec2 cNoiseSpeed;
uniform float cNoiseTiling;
#endif
#ifdef COMPILEPS
uniform float cNoiseStrength;
uniform float cFresnelPower;
uniform vec3 cWaterTint;
uniform vec3 cShallowColor;
uniform vec3 cDeepColor;
uniform float cDepthScale;
#endif

void VS()
{
    mat4 modelMatrix = iModelMatrix;
    vec3 worldPos = GetWorldPos(modelMatrix);
    gl_Position = GetClipPos(worldPos);
    vScreenPos = GetScreenPos(gl_Position);
    // GetQuadTexCoord() returns a vec2 that is OK for quad rendering; multiply it with output W
    // coordinate to make it work with arbitrary meshes such as the water plane (perform divide in pixel shader)
    // Also because the quadTexCoord is based on the clip position, and Y is flipped when rendering to a texture
    // on OpenGL, must flip again to cancel it out
    vReflectUV = GetQuadTexCoord(gl_Position);
    vReflectUV.y = 1.0 - vReflectUV.y;
    vReflectUV *= gl_Position.w;
    vWaterUV = iTexCoord * cNoiseTiling + cElapsedTime * cNoiseSpeed;
    vNormal = GetWorldNormal(modelMatrix);
    vEyeVec = vec4(cCameraPos - worldPos, GetDepth(gl_Position));
}

void PS()
{
    vec2 refractUV = vScreenPos.xy / vScreenPos.w;
    vec2 reflectUV = vReflectUV.xy / vScreenPos.w;

    vec2 noise = (texture2D(sNormalMap, vWaterUV).rg - 0.5) * cNoiseStrength;
    refractUV += noise;
    // Do not shift reflect UV coordinate upward, because it will reveal the clipping of geometry below water
    if (noise.y < 0.0)
        noise.y = 0.0;
    reflectUV += noise;

    vec3 refractColor = texture2D(sEnvMap, refractUV).rgb * cWaterTint;
    vec3 reflectColor = texture2D(sDiffMap, reflectUV).rgb;

    float eyeDotN = dot(normalize(vEyeVec.xyz), vNormal);

    vec3 finalColor;
    if (eyeDotN < 0.0)
    {
        // Viewed from below — use the viewport/refraction texture which shows what's behind
        // the water surface from the camera's viewpoint (sky, above-water terrain).
        // The reflection texture UV mapping is designed for above-water viewing and maps
        // incorrectly from below, so we avoid it entirely.
        // Snell's window: looking straight up = see through, grazing = subtle water tint
        float transparency = clamp(-eyeDotN, 0.0, 1.0);
        vec3 waterColor = mix(cShallowColor, cDeepColor, 0.5);
        float tint = 0.15 * (1.0 - transparency);
        finalColor = mix(refractColor, waterColor, tint);
    }
    else
    {
        float fresnel = pow(1.0 - clamp(eyeDotN, 0.0, 1.0), cFresnelPower);
        fresnel = min(fresnel, 0.7);

        // Depth-based water color: steep view = deep, grazing = shallow
        float depthFactor = clamp(eyeDotN * cDepthScale, 0.0, 1.0);
        vec3 waterColor = mix(cShallowColor, cDeepColor, depthFactor);

        // Blend refraction with water color — more tint at grazing angles, less when looking straight down
        // so submerged objects in shallow water remain visible from above
        float tintAmount = 0.15 * (1.0 - eyeDotN);
        vec3 tintedRefract = mix(refractColor, waterColor, tintAmount);
        finalColor = mix(tintedRefract, reflectColor, fresnel);
    }

    gl_FragColor = vec4(GetFog(finalColor, GetFogFactor(vEyeVec.w)), 1.0);
}
