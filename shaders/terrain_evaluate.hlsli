#include "terrain_noise.hlsli"
#include "terrain_erosion.hlsli"

// Differential scalars: value, d/d(world X), d/d(world Z). All gradients
// include field frequencies, blend weights, and octave feedback.
float3 product(float3 a, float3 b) {
    return float3(a.x*b.x, a.yz*b.x+b.yz*a.x);
}
float3 square(float3 a) { return product(a,a); }
float3 unitClamp(float3 a) {
    return float3(saturate(a.x), a.x>0 && a.x<1 ? a.yz : float2(0,0));
}
// Material classification needs a continuous slope, whereas generation keeps
// its original ridged height function. Round the feedback saturation knee:
// value and derivative join the linear/constant regions at 0.8 and 1.2.
// Input is nonnegative (squared ridge times nonnegative previous feedback).
float3 materialFeedback(float3 a) {
    float t=saturate((a.x-0.8)*2.5);
    return float3(min(a.x,1.2)-0.2*t*t,a.yz*(1-t));
}
float3 worldNoise(uint domain, float2 local) {
    float3 n=gradientNoise(domain,local);
    n.yz*=params.domains[domain].fractionFrequency.z;
    return n;
}
float3 twoOctaves(uint first, float2 local) {
    return (worldNoise(first,local)+0.5*worldNoise(first+1,local))/1.5;
}
float3 roundedRidge(float3 n, float rounding) {
    float len=sqrt(n.x*n.x+rounding*rounding);
    float norm=sqrt(1+rounding*rounding)-rounding;
    return square(unitClamp(float3(1-(len-rounding)/norm,-n.x/len/norm*n.yz)));
}
struct BaseTerrainSample {
    float height;
    float2 gradient;
    float mountainWeight;
    float peakValley;
    float2 erosionGradient;
};
BaseTerrainSample evaluateBaseTerrain(float2 local, bool materialSlope = false) {
    float3 elevation=twoOctaves(4,local)*0.5+float3(0.5,0,0);
    float3 t=unitClamp(twoOctaves(6,local)/0.2);
    float x=t.x;
    float3 m=unitClamp(float3(x*x*x*(x*(x*6-15)+10),t.yz*(30*x*x*(x-1)*(x-1))));
    float3 localForms=0, coarseForms=0, ridges=0, driverRidges=0, feedback=float3(1,0,0);
    float ridgeStrength=params.geography.y;
    float q=0.5*params.geography.z;
    float normalization=1+q+q*q+q*q*q;
    float coarseNormalization=1+q;
    float amplitude=1;
    [unroll] for(uint i=0;i<4;++i) {
        // Uniform setting: the smooth endpoint needs only the broadest octave.
        // Skip expensive noise/hash work rather than multiplying it by zero.
        [branch] if(i>0 && q==0) continue;
        float3 n=worldNoise(i,local);
        localForms+=n*amplitude;
        if(i<2) {
            coarseForms+=n*amplitude;
            if(ridgeStrength>0)
                driverRidges+=roundedRidge(n,0.15)*amplitude;
        }
        // Only the visible ridge function uses fine feedback; the erosion
        // direction field remains limited to broad, rounded features.
        if(ridgeStrength>0) {
            float3 ridge=roundedRidge(n,0.02);
            float3 signal=product(ridge,feedback);
            ridges+=signal*amplitude;
            feedback=materialSlope ? materialFeedback(signal*2) : unitClamp(signal*2);
        }
        amplitude*=q;
    }
    float3 plains=localForms/normalization;
    float3 hills=plains*0.5+float3(0.5,0,0);
    coarseForms/=coarseNormalization;
    float3 coarseHills=coarseForms*0.5+float3(0.5,0,0);
    // Explicit endpoint keeps the original ridge arithmetic at (1,1).
    float3 relief=ridgeStrength==1 ? square(ridges/normalization) :
        lerp(square(hills),square(ridges/normalization),ridgeStrength);
    float3 driverRelief=ridgeStrength==1 ? square(driverRidges/coarseNormalization) :
        lerp(square(coarseHills),square(driverRidges/coarseNormalization),ridgeStrength);
    float3 inverse=float3(1,0,0)-m;
    float3 h=elevation*0.1 + product(square(inverse),plains)*(1.0/24.0)
        + product(product(m,inverse)*2,hills)*0.1 + product(square(m),relief);
    float3 driver=elevation*0.1 + product(square(inverse),coarseForms)*(1.0/24.0)
        + product(product(m,inverse)*2,coarseHills)*0.1
        + product(square(m),driverRelief);
    BaseTerrainSample result;
    result.height=h.x*params.shape.y;
    result.gradient=h.yz*params.shape.y;
    result.mountainWeight=m.x;
    result.peakValley=driverRelief.x*2-1;
    result.erosionGradient=driver.yz*params.shape.y;
    return result;
}
float2 evaluateTerrain(float2 local, BaseTerrainSample base) {
    float amplitude=params.shape.y/3;
    if(params.shape.z==0 || amplitude==0 || base.mountainWeight==0)
        return base.height.xx;
    // erode is translation invariant in height: evaluate only displacement.
    float3 input=float3(0,base.erosionGradient*(params.geography.x/amplitude));
    float displacement=erode(local,input,base.peakValley)*amplitude*base.mountainWeight;
    return float2(base.height,base.height+displacement);
}

float2 evaluateTerrain(float2 local) {
    return evaluateTerrain(local,evaluateBaseTerrain(local));
}
