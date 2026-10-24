#ifndef OW_MATERIAL_SAMPLING
#define OW_MATERIAL_SAMPLING
// All samples are explicitly differentiated before any per-layer/axis branch.
struct MicroSample { float3 colour; float2 slope; float4 scalar; };
struct LayerSample { float3 colour, gradient; float4 scalar; float reads; };
struct MaterialEvaluation { SurfaceMaterial surface; float4 weights; float3 projections; float reads; };
MicroSample microSample(MaterialRecord record,float2 uv,float2 dx,float2 dy) {
    float3 tc=float3(uv,record.maps.w); float4 c,n,s;
#ifdef OW_BINDLESS
    c=materialArrays[NonUniformResourceIndex(record.maps.x)].SampleGrad(materialSampler,tc,dx,dy);
    n=materialArrays[NonUniformResourceIndex(record.maps.y)].SampleGrad(materialSampler,tc,dx,dy);
    s=materialArrays[NonUniformResourceIndex(record.maps.z)].SampleGrad(materialSampler,tc,dx,dy);
#else
    c=materialColour.SampleGrad(materialSampler,tc,dx,dy);
    n=materialNormal.SampleGrad(materialSampler,tc,dx,dy);
    s=materialScalar.SampleGrad(materialSampler,tc,dx,dy);
#endif
    MicroSample result; result.colour=c.rgb; result.scalar=s;
    // Quantized neutral UNORM is 128; absorb its half-texel bias. Authored
    // slopes outside this tiny dead zone retain their full signed direction.
    float2 xy=n.xy*2-1;
    xy=sign(xy)*max(abs(xy)-1.0/255.0,0);
    result.slope=xy/max(sqrt(saturate(1-dot(xy,xy))),.1);
    return result;
}
float4 macroSample(MaterialRecord record,float2 uv,float2 dx,float2 dy) {
#ifdef OW_BINDLESS
    return materialArrays[NonUniformResourceIndex(record.macro.x)].SampleGrad(macroSampler,float3(uv,record.maps.w),dx,dy);
#else
    return materialMacro.SampleGrad(macroSampler,float3(uv,record.maps.w),dx,dy);
#endif
}
float3 projectionWeights(float3 n) {
    if(materialInfo.features.x==0)return float3(0,1,0);
    // Full triplanar by 60 degrees, top projection below 25 degrees.
    float cliff=smoothstep(.0936922,.5,1-abs(n.y));
    float3 p=pow(abs(n),4);
    p/=max(dot(p,1.0.xxx),1e-6);
    if(materialInfo.features.y!=0) {
        uint axis=p.x<p.y?(p.x<p.z?0:2):(p.y<p.z?1:2);
        p[axis]*=smoothstep(.01,.08,p[axis]);
        p/=max(dot(p,1.0.xxx),1e-6);
    }
    return lerp(float3(0,1,0),p,cliff);
}
float4 heightWeights(float4 coverage,float4 heights,float influence,float width) {
    float sum=dot(coverage,1.0.xxxx);
    float4 base=sum>1e-6?coverage/sum:float4(1,0,0,0);
    float4 scores=base+.25*(saturate(heights)-.5);
    float peak=-1e10;
    [unroll]for(uint i=0;i<4;++i)if(base[i]>0)peak=max(peak,scores[i]);
    float4 adjusted=base*max(scores-peak+width,0);
    float total=dot(adjusted,1.0.xxxx);
    adjusted=total>1e-6?adjusted/total:base;
    return lerp(base,adjusted,saturate(influence));
}
MaterialEvaluation evaluateMappedMaterial(float3 normal,float3 relative,float4 coverage,float3 pdx,float3 pdy) {
    MaterialEvaluation result=(MaterialEvaluation)0;
    normal=normalize(normal);
    float3 micro=relative*materialInfo.coordinates.microPhase.w+materialInfo.coordinates.microPhase.xyz;
    float3 macro=relative*materialInfo.coordinates.macroPhase.w+materialInfo.coordinates.macroPhase.xyz;
    float footprint=max(length(pdx),length(pdy))*materialInfo.coordinates.microPhase.w;
    float detail=1-materialInfo.blend.w*smoothstep(.03125,.25,footprint);
    float normalScale=materialInfo.controls.x*(4*materialInfo.coordinates.microPhase.w)*detail;
    float3 projections=projectionWeights(normal);
    LayerSample layers[4];float4 heights=0;
    if(dot(coverage,1.0.xxxx)<=1e-6)coverage=float4(1,0,0,0);
    [loop]for(uint layer=0;layer<4;++layer) {
        layers[layer]=(LayerSample)0;
        [branch]if(coverage[layer]<=0)continue;
        MaterialRecord record=materialRecords[layer+1];
        float3 axes=layer==2?projections:float3(0,1,0);
        [loop]for(uint axis=0;axis<3;++axis) {
            [branch]if(axes[axis]<=0)continue;
            float signAxis=normal[axis]<0?-1:1;
            // Left-handed U/V/N, matching the importer's +U/+V convention.
            float3 U=axis==0?float3(0,0,signAxis):axis==1?float3(1,0,0):float3(-signAxis,0,0);
            float3 V=axis==1?float3(0,0,signAxis):float3(0,1,0);
            float2 uv=float2(dot(micro,U),dot(micro,V));
            float2 dx=float2(dot(pdx,U),dot(pdx,V))*materialInfo.coordinates.microPhase.w;
            float2 dy=float2(dot(pdy,U),dot(pdy,V))*materialInfo.coordinates.microPhase.w;
            MicroSample s=microSample(record,uv,dx,dy);
            float reads=3;
            [branch]if(materialInfo.blend.z>0) {
                float2 mdx=float2(dot(pdx,U),dot(pdx,V))*materialInfo.coordinates.macroPhase.w;
                float2 mdy=float2(dot(pdy,U),dot(pdy,V))*materialInfo.coordinates.macroPhase.w;
                float4 m=macroSample(record,float2(dot(macro,U),dot(macro,V)),mdx,mdy);
                float3 variation=clamp((m.rgb*2-1)*.4,-.2,.2);
                s.colour*=1+variation*materialInfo.blend.z;
                s.scalar.r+=clamp((m.a*2-1)*.08,-.08,.08)*materialInfo.blend.z;
                reads+=1;
            }
            layers[layer].colour+=s.colour*axes[axis];
            layers[layer].scalar+=s.scalar*axes[axis];
            // Project the volume gradient into the actual surface tangent plane.
            float3 gradient=-(U*s.slope.x+V*s.slope.y)*normalScale;
            layers[layer].gradient+=(gradient-normal*dot(normal,gradient))*axes[axis];
            layers[layer].reads+=reads;
        }
        heights[layer]=layers[layer].scalar.b;
        result.reads+=layers[layer].reads;
    }
    result.weights=heightWeights(coverage,heights,materialInfo.blend.x*detail,materialInfo.blend.y);
    result.projections=projections;
    float3 colour=0,gradient=0;float4 scalar=0;
    [unroll]for(uint i=0;i<4;++i) {
        colour+=layers[i].colour*result.weights[i];
        scalar+=layers[i].scalar*result.weights[i];
        gradient+=layers[i].gradient*result.weights[i];
    }
    result.surface=terrainMaterial(normal);
    result.surface.baseColour=colour*materialInfo.tint.rgb;
    result.surface.roughness=clamp(scalar.r*materialInfo.controls.y,.045,1);
    result.surface.ao=saturate(scalar.g);result.surface.metallic=0;result.surface.emissive=0;
    result.surface.normal=normalize(normal-gradient);
    return result;
}
MaterialEvaluation evaluateMappedMaterial(float3 normal,float3 relative,float4 coverage) {
    return evaluateMappedMaterial(normal,relative,coverage,ddx(relative),ddy(relative));
}
#endif
