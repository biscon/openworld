// Advanced Terrain Erosion Filter and Phacelle Noise
// Copyright (c) 2025 Rune Skovbo Johansen; C# translation (c) 2026 Luke Mitchell.
// HLSL adaptation for OpenWorld. See third_party/erosion for provenance/notices.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain
// one at https://mozilla.org/MPL/2.0/.
float easeOut(float t) { float v = 1.0-saturate(t); return 1.0-v*v; }
float smoothStart(float t, float smoothing) {
    return t >= smoothing ? t - 0.5*smoothing : 0.5*t*t/max(smoothing,1e-10);
}
float2 safeDirection(float2 v) { return v / max(length(v), 1e-10); }
float4 phacelle(uint octave, float2 local, float2 direction) {
    float2 side = float2(-direction.y, direction.x) * (0.7 * 6.28318530718);
    int2 cell; float2 f;
    domainPoint(8+octave, local, cell, f);
    float2 phase = 0; float weights = 0;
    [unroll] for (int z=-1; z<=2; ++z) [unroll] for (int x=-1; x<=2; ++x) {
        int2 offset = int2(x,z);
        float2 v = f-float2(offset)-hashCell(cell+offset)*0.5;
        float w = max(0.0, exp(-dot(v,v)*2.0)-0.01111);
        float wave = dot(v,side)+1.57079632679;
        float s,c; sincos(wave,s,c);
        phase += float2(c,s)*w; weights += w;
    }
    phase /= max(weights,1e-10);
    phase /= max(0.5,length(phase));
    return float4(phase,side);
}
float erode(float2 local, float3 terrain, float fadeTarget) {
    const float4 rounding = float4(0.1,params.shape.w,0.1,2.0);
    float slopeLength = max(length(terrain.yz),1e-10);
    float roundInput = lerp(rounding.y,rounding.x,saturate(fadeTarget+0.5))*rounding.z;
    float mask = easeOut(smoothStart(slopeLength*1.25, roundInput*1.25));
    float2 gullySlope = terrain.yz / slopeLength * 0.7;
    float strength = params.shape.z * 0.15;
    float roundMult = 1.0, delta = 0, magnitude = 0;
    [unroll] for (uint i=0;i<5;++i) {
        float4 p = phacelle(i,local,safeDirection(gullySlope));
        // Slope is measured in normalized terrain coordinates (one erosion-reference wavelength).
        p.zw *= -(params.domains[8+i].fractionFrequency.z * params.geography.x);
        gullySlope += sign(p.y)*p.zw*strength*0.5;
        float faded = lerp(fadeTarget,p.x*0.5,mask);
        delta += faded*strength; magnitude += strength;
        fadeTarget = faded;
        float roundOctave = lerp(rounding.y,rounding.x,saturate(p.x+0.5))*roundMult;
        mask = (1.0-pow(1.0-saturate(mask),1.5))*easeOut(smoothStart(abs(p.y)*1.25,roundOctave*1.25));
        strength *= 0.5; roundMult *= rounding.w;
    }
    return terrain.x + delta - 0.65*magnitude;
}
