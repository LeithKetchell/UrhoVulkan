#include "Uniforms.glsl"
#include "Samplers.glsl"
#include "Transform.glsl"

#if defined(DIFFMAP) || defined(ALPHAMAP)
    varying vec2 vTexCoord;
#endif
#ifdef VERTEXCOLOR
    varying vec4 vColor;
#endif
varying float vVertexIndex;

void VS()
{
    // DIAGNOSTIC: Output iPos directly to test vertex data reading
    // UI vertices are in screen space (0-1024, 0-768), scale to NDC manually
    vec4 pos = iPos;
    gl_Position = vec4(pos.x / 512.0 - 1.0, pos.y / 384.0 - 1.0, 0.5, 1.0);

    #ifdef DIFFMAP
        vTexCoord = iTexCoord;
    #endif
    #ifdef VERTEXCOLOR
        vColor = iColor;
    #endif

    vVertexIndex = float(gl_VertexIndex);
}

void PS()
{
    // DIAGNOSTIC: Color by vertex index to test if vertex shader runs
    float idx = vVertexIndex / 6.0;  // Normalize to 0-1 for first 6 vertices
    gl_FragColor = vec4(idx, 1.0 - idx, 0.5, 1.0);
    return;

    vec4 diffColor = cMatDiffColor;

    #ifdef VERTEXCOLOR
        diffColor *= vColor;
    #endif

    // DIAGNOSTIC: Output diffColor without texture (white * vertex color)
    gl_FragColor = diffColor;
    return;

    #if (!defined(DIFFMAP)) && (!defined(ALPHAMAP))
        gl_FragColor = diffColor;
    #endif
    #ifdef DIFFMAP
        vec4 diffInput = texture2D(sDiffMap, vTexCoord);
        #ifdef ALPHAMASK
            if (diffInput.a < 0.5)
                discard;
        #endif
        gl_FragColor = diffColor * diffInput;
    #endif
    #ifdef ALPHAMAP
        #ifdef GL3
            float alphaInput = texture2D(sDiffMap, vTexCoord).r;
        #else
            float alphaInput = texture2D(sDiffMap, vTexCoord).a;
        #endif
        gl_FragColor = vec4(diffColor.rgb, diffColor.a * alphaInput);
    #endif
}
