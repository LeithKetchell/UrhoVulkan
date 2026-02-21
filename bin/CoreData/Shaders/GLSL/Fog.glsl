#ifdef COMPILEPS
vec3 GetFog(vec3 color, float fogFactor)
{
    return mix(cFogColor, color, fogFactor);
}

vec3 GetLitFog(vec3 color, float fogFactor)
{
    return color * fogFactor;
}

float GetFogFactor(float depth)
{
    return clamp((cFogParams.x - depth) * cFogParams.y, 0.0, 1.0);
}

float GetHeightFogFactor(float depth, float height)
{
    float fogFactor = GetFogFactor(depth);
    float rawHeight = (height - cFogParams.z) * cFogParams.w;
    if (rawHeight <= 0.0)
        return 1.0;
    float heightFogFactor = clamp(exp(-(rawHeight * rawHeight)), 0.0, 1.0);
    return min(heightFogFactor, fogFactor);
}
#endif
